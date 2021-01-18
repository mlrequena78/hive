#include <hive/plugins/account_history_sql/account_history_sql_plugin.hpp>

#include <pqxx/pqxx>

#include <chrono>

namespace hive
{
 namespace plugins
 {
  namespace account_history_sql
  {
    using chain::database;
    using hive::utilities::postgres_connection_holder;
    using protocol::operation;

    using hive::utilities::postgres_connection_pool;

    namespace detail
    {
      class time_logger
      {
        bool val = true;

        std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
        std::chrono::time_point<std::chrono::high_resolution_clock> finish_time;
        std::chrono::time_point<std::chrono::high_resolution_clock> finish2_time;

        public:

          time_logger()
          {
            start_time = std::chrono::high_resolution_clock::now();
          }

          void snapshot_time()
          {
            if( val )
              finish_time = std::chrono::high_resolution_clock::now();
            else
              finish2_time = std::chrono::high_resolution_clock::now();
            val = false;
          }

          void info()
          {
            std::chrono::milliseconds sql_time = std::chrono::duration_cast<std::chrono::milliseconds>( finish_time - start_time );
            std::chrono::milliseconds cpp_time = std::chrono::duration_cast<std::chrono::milliseconds>( finish2_time - finish_time );

            ilog( "SQL AH statistics: sql time: ${sql}ms cpp time: ${cpp}ms", ("sql", sql_time.count())("cpp", cpp_time.count()) );
          }
      };

      class account_history_sql_plugin_impl
      {
        using reversible_operation = sql_serializer::PSQL::processing_objects::volatile_process_operation_flag_t;

        public:
          account_history_sql_plugin_impl( const std::string &url, uint32_t thread_pool_size )
            : _db( appbase::app().get_plugin< hive::plugins::chain::chain_plugin >().db() ), connection{url}, pool( url, thread_pool_size ),
            sql_serializer( appbase::app().get_plugin<hive::plugins::sql_serializer::sql_serializer_plugin>() )
          {
          }

          virtual ~account_history_sql_plugin_impl()
          {
            ilog("finishing account_history_sql_plugin_impl destructor...");
          }

          bool get_ops_in_block_reversible( std::vector<account_history_sql_object>& reversible_results,
                                            uint32_t block_number,
                                            std::function<bool(const reversible_operation&)> filter, const fc::optional<uint32_t>& limit = fc::optional<uint32_t>() );

          void get_ops_in_block( account_history_sql::account_history_sql_plugin::sql_result_type sql_result,
                                uint32_t block_number, bool only_virtual, const fc::optional<bool>& include_reversible );

          bool get_transaction_reversible( hive::protocol::annotated_signed_transaction& sql_result,
                                const hive::protocol::transaction_id_type& id, const fc::optional<bool>& include_reversible );

          void get_transaction( hive::protocol::annotated_signed_transaction& sql_result,
                                const hive::protocol::transaction_id_type& id, const fc::optional<bool>& include_reversible );

          bool get_account_history_reversible( account_history_sql::account_history_sql_plugin::sql_sequence_result_type sql_sequence_result,
                                    const hive::protocol::account_name_type& account, uint64_t start, uint32_t limit,
                                    const fc::optional<bool>& include_reversible,
                                    const fc::optional<uint64_t>& operation_filter_low,
                                    const fc::optional<uint64_t>& operation_filter_high );

          void get_account_history( account_history_sql::account_history_sql_plugin::sql_sequence_result_type sql_sequence_result,
                                    const hive::protocol::account_name_type& account, uint64_t start, uint32_t limit,
                                    const fc::optional<bool>& include_reversible,
                                    const fc::optional<uint64_t>& operation_filter_low,
                                    const fc::optional<uint64_t>& operation_filter_high );

          void enum_virtual_ops_reversible( std::vector< account_history_sql_object >& reversible_results,
                                            uint32_t block_range_begin, uint32_t block_range_end,
                                            const fc::optional<bool>& include_reversible,
                                            int64_t operation_begin, uint32_t limit,
                                            const std::set<uint32_t>& filter );

          void enum_virtual_ops_reversible_find_next_elements( uint32_t& next_block_range_begin, uint64_t& next_operation_begin,
                                            uint32_t start_block,
                                            int64_t operation_begin,
                                            const std::set<uint32_t>& filter );

          void enum_virtual_ops_irreversible_find_next_elements( uint32_t& next_block_range_begin, uint64_t& next_operation_begin,
                                            uint32_t start_block,
                                            int64_t operation_begin,
                                            const std::string& filter );

          void enum_virtual_ops( account_history_sql::account_history_sql_plugin::sql_result_type sql_result,
                                uint32_t block_range_begin, uint32_t block_range_end,
                                const fc::optional<bool>& include_reversible,
                                const fc::optional<uint64_t>& operation_begin, const fc::optional< uint32_t >& limit,
                                const fc::optional<uint32_t>& filter,
                                uint32_t& next_block_range_begin,
                                uint64_t& next_operation_begin );

        private:

          database &_db;

          postgres_connection_holder connection;
          postgres_connection_pool pool;

          void synchronize( uint32_t blockRangeBegin, uint32_t blockRangeEnd );
          bool reversible_is_required( const fc::optional<bool>& include_reversible );
          bool execute_query( const std::string& query, pqxx::result& result );
          void fill_object( const pqxx::tuple& row, account_history_sql_object& obj, fc::optional<uint32_t>block_number );
          void create_filter_array( const std::set<uint32_t>& v, std::string& filter_array );
          void gather_operations_from_filter( const fc::optional<uint64_t>& filter, std::set<uint32_t>& s, bool high );
          void gather_virtual_operations_from_filter( const fc::optional<uint32_t>& filter, std::set<uint32_t>& s );

          hive::plugins::sql_serializer::sql_serializer_plugin& sql_serializer;

      };//class account_history_sql_plugin_impl

      void account_history_sql_plugin_impl::synchronize( uint32_t blockRangeBegin, uint32_t blockRangeEnd )
      {
        FC_ASSERT(blockRangeBegin < blockRangeEnd, "Wrong block range");

        if(sql_serializer.get_currently_persisted_irreversible_block() >= blockRangeBegin && sql_serializer.get_currently_persisted_irreversible_block() <= blockRangeEnd)
        {
          ilog("Awaiting for the end of save current irreversible block ${b} block, requested by call: [${rb}, ${re}]",
            ("b", sql_serializer.get_currently_persisted_irreversible_block().operator unsigned int())("rb", blockRangeBegin)("re", blockRangeEnd));

          /** Api requests data of currently saved irreversible block, so it must wait for the end of storage and cleanup of 
          *   volatile ops container.
          */
          std::unique_lock<std::mutex> lk(sql_serializer.get_currently_persisted_irreversible_mtx());

          sql_serializer.get_currently_persisted_irreversible_cv().wait(lk,
            [this, blockRangeBegin, blockRangeEnd]() -> bool
            {
              return sql_serializer.get_currently_persisted_irreversible_block() < blockRangeBegin || sql_serializer.get_currently_persisted_irreversible_block() > blockRangeEnd;
            }
          );

          ilog("Resumed evaluation a range containing currently just written irreversible block, requested by call: [${rb}, ${re}]",
            ("rb", blockRangeBegin)("re", blockRangeEnd));
        }
      }

      bool account_history_sql_plugin_impl::reversible_is_required( const fc::optional<bool>& include_reversible )
      {
        return include_reversible.valid() ? *include_reversible : false;
      }

      bool account_history_sql_plugin_impl::execute_query( const std::string& query, pqxx::result& result )
      {
        auto conn = pool.get_connection();

        FC_ASSERT( conn.conn, "A connection must exist" );

        try
        {
          if( !connection.exec_query( *( conn.conn ), query, &result ) )
          {
            wlog( "Execution of query ${query} failed", ("query", query) );
            return false;
          }

          pool.restore_connection( conn );

          return true;
        }
        catch( const fc::exception& e )
        {
          elog( "Caught exception in AH SQL plugin: ${e}", ("e", e.to_detail_string() ) );
          pool.restore_connection( conn );
        }
        catch( const boost::exception& e )
        {
          elog( "Caught exception in AH SQL plugin: ${e}", ("e", boost::diagnostic_information(e)) );
          pool.restore_connection( conn );
        }
        catch( const std::exception& e )
        {
          elog( "Caught exception in AH SQL plugin: ${e}", ("e", e.what()));
          pool.restore_connection( conn );
        }
        catch( ... )
        {
          wlog( "Caught exception in AH SQL plugin" );
          pool.restore_connection( conn );
        }

        return false;
      }

      void account_history_sql_plugin_impl::fill_object( const pqxx::tuple& row, account_history_sql_object& obj, fc::optional<uint32_t>block_number = fc::optional<uint32_t>() )
      {
          uint32_t cnt = 0;

          obj.trx_id        = hive::protocol::transaction_id_type( row[ cnt++ ].as< std::string >() );
          obj.block         = block_number.valid() ? ( *block_number ) : row[ cnt++ ].as< uint32_t >();

          obj.trx_in_block  = row[ cnt++ ].as< uint32_t >();

          obj.op_in_trx     = row[ cnt++ ].as< uint32_t >();
          obj.virtual_op    = static_cast< uint32_t >( row[ cnt++ ].as< bool >() );
          obj.timestamp     = fc::time_point_sec::from_iso_string( row[ cnt++ ].as< std::string >() );

          obj.op            = fc::strjson( { row[ cnt++ ].as< std::string >() } );

          obj.operation_id = row[ cnt++ ].as< uint64_t >();
      }

      void account_history_sql_plugin_impl::create_filter_array( const std::set<uint32_t>& s, std::string& filter_array )
      {
        filter_array = "ARRAY[ ";
        bool _first = true;

        for( auto val : s )
        {
          filter_array += _first ? std::to_string( val ) : ( ", " + std::to_string( val ) );
          _first = false;
        }
        filter_array += " ]::INT[]";
      }

      void account_history_sql_plugin_impl::gather_operations_from_filter( const fc::optional<uint64_t>& filter, std::set<uint32_t>& s, bool high )
      {
        if( !filter.valid() )
          return;

        uint64_t _val = *filter;
        for( uint64_t it = 0; it < 64; ++it )
        {
          if( _val & 1 )
            s.emplace( high ? ( it + 64 ) : it );
          _val >>= 1;
        }
      }

      void account_history_sql_plugin_impl::gather_virtual_operations_from_filter( const fc::optional<uint32_t>& filter, std::set<uint32_t>& s )
      {
        if( !filter.valid() )
          return;

        //In postgres there are put not virtual operations before virtual operation. It's necessary to make a shift.
        const uint32_t not_virtual_shift = 48;

        uint32_t _val = *filter;
        for( uint32_t it = 0; it < 32; ++it )
        {
          if( _val & 1 )
            s.emplace( it + not_virtual_shift );
          _val >>= 1;
        }
      }

      bool account_history_sql_plugin_impl::get_ops_in_block_reversible( std::vector<account_history_sql_object>& reversible_results,
                                                                          uint32_t block_number,
                                                                          std::function<bool(const reversible_operation&)> filter, const fc::optional<uint32_t>& limit )
      {
        synchronize( block_number, block_number + 1 );

        const auto& idx_block = _db.get_index<sql_serializer::PSQL::volatile_block_index, sql_serializer::PSQL::by_block>();
        auto itr_block = idx_block.find( block_number );

        if( itr_block == idx_block.end() )
          return false;

        const sql_serializer::PSQL::processing_objects::process_block_t& content_block = itr_block->content;

        const auto& idx_transaction = _db.get_index<sql_serializer::PSQL::volatile_transaction_index, sql_serializer::PSQL::by_block_trx_in_block>();
        const auto& idx_operation = _db.get_index<sql_serializer::PSQL::volatile_operation_index, sql_serializer::PSQL::by_block>();

        auto itr_operation = idx_operation.find( block_number );

        const hive::protocol::transaction_id_type empty_hash( "0000000000000000000000000000000000000000" );
        hive::protocol::transaction_id_type hash;
        int16_t last_trx_in_block = -2;
        uint32_t cnt = 0;

        while( itr_operation != idx_operation.end() && itr_operation->get_block_number() == block_number )
        {
          account_history_sql_object ah_obj;
          const reversible_operation& content_operation = itr_operation->content;

          if( !filter( content_operation ) )
          {
            ++itr_operation;
            continue;
          }

          if( content_operation.trx_in_block != last_trx_in_block )
          {
            auto itr_transaction = idx_transaction.find( boost::make_tuple( block_number, content_operation.trx_in_block ) );
            if( itr_transaction == idx_transaction.end() )
              hash = empty_hash;
            else
              hash = itr_transaction->content.hash;
            last_trx_in_block = content_operation.trx_in_block;
          }

          ah_obj.trx_id        = hash;
          ah_obj.block         = block_number;
          ah_obj.trx_in_block  = content_operation.trx_in_block;
          ah_obj.op_in_trx     = content_operation.op_in_trx;
          ah_obj.virtual_op    = content_operation.is_virtual;
          ah_obj.timestamp     = content_block.created_at;

          operation _op = fc::raw::unpack_from_buffer< hive::protocol::operation >( content_operation.op );
          fc::variant opVariant;
          fc::to_variant(_op, opVariant);
          ah_obj.op = opVariant;

          ah_obj.operation_id = content_operation.operation_id;

          reversible_results.emplace_back( ah_obj );

          cnt++;
          if( limit.valid() && cnt == *limit )
            break;

          ++itr_operation;
        }

        return true;
      }

      void account_history_sql_plugin_impl::get_ops_in_block( account_history_sql::account_history_sql_plugin::sql_result_type sql_result,
                                                              uint32_t block_number, bool only_virtual, const fc::optional<bool>& include_reversible )
      {
        bool _reversible_is_required = reversible_is_required( include_reversible );

        std::vector< account_history_sql_object > reversible_results;

        auto _filter = [ only_virtual ]( const reversible_operation& r_op )
        {
          if( only_virtual )
            return r_op.is_virtual;
          else
            return true;
        };

        if( _reversible_is_required )
        {
          if( get_ops_in_block_reversible( reversible_results, block_number, _filter ) )
          {
            for( auto& op : reversible_results )
            {
              sql_result( op );
            }
            return;
          }
        }

        const uint32_t NR_FIELDS = 7;

        pqxx::result result;
        std::string sql;

        std::string virtual_mode = only_virtual ? "TRUE" : "FALSE";

        time_logger _time_logger;

        sql = "select * from ah_get_ops_in_block( " + std::to_string( block_number ) + ", " + virtual_mode + " ) order by _trx_in_block, _virtual_op;";

        if( !execute_query( sql, result ) )
          return;

        _time_logger.snapshot_time();

        for( const auto& row : result )
        {
          account_history_sql_object ah_obj;

          FC_ASSERT( row.size() == NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields",
                                              ( "db_size", row.size() )( "req_size", NR_FIELDS ) );

          fill_object( row, ah_obj, block_number );

          sql_result( ah_obj );
        }

        _time_logger.snapshot_time();
        _time_logger.info();
      }

      bool account_history_sql_plugin_impl::get_transaction_reversible( hive::protocol::annotated_signed_transaction& sql_result,
                                                            const hive::protocol::transaction_id_type& id, const fc::optional<bool>& include_reversible )
      {
        //Returning reversible blocks not supported therefore the call `get_transaction` always returns irreversible data.
        return false;
      }

      void account_history_sql_plugin_impl::get_transaction( hive::protocol::annotated_signed_transaction& sql_result,
                                                            const hive::protocol::transaction_id_type& id, const fc::optional<bool>& include_reversible )
      {
        if( get_transaction_reversible( sql_result, id, include_reversible ) )
          return;

        const uint32_t TRX_NR_FIELDS = 7;
        const uint32_t OP_NR_FIELDS = 1;
        const uint32_t MULTISIG_NR_FIELDS = 1;
        const uint32_t NR_CHARS_IN_SIGNATURE = 130;

        pqxx::result result;
        std::string sql;

        time_logger _time_logger;

        sql = "select * from ah_get_trx( '"+ id.str() + "' )";

        if( !execute_query( sql, result ) )
          return;

        FC_ASSERT( result.size() == 1 , "The database returned ${db_size} records, but there is required only 1 record",
                                        ( "db_size", result.size() ) );

        const auto& row = result[0];
        FC_ASSERT( row.size() == TRX_NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields",
                                                ( "db_size", row.size() )( "req_size", TRX_NR_FIELDS ) );

        std::vector<hive::protocol::signature_type> signatures;
        uint32_t cnt = 0;

        sql_result.ref_block_num    = row[ cnt++ ].as< uint32_t >();
        sql_result.ref_block_prefix = row[ cnt++ ].as< uint32_t >();
        sql_result.expiration       = fc::time_point_sec::from_iso_string( row[ cnt++ ].as< std::string >() );
        sql_result.block_num        = row[ cnt++ ].as< uint32_t >();
        sql_result.transaction_num  = row[ cnt++ ].as< uint32_t >();
        std::string signature       = row[ cnt++ ].as< std::string >();
        uint32_t multisig_num      = row[ cnt++ ].as< uint32_t >();

        sql_result.transaction_id   = id;

        auto create_signature = [ &signatures ]( const std::string& signature )
        {
          if( !signature.empty() && signature.size() == NR_CHARS_IN_SIGNATURE )
          {
            hive::protocol::signature_type sig;
            size_t cnt = 0;
            for( size_t i = 0; i < signature.size(); i += 2 )
            {
              std::string s = { signature[i], signature[i+1] };
              sig.begin()[ cnt ] = static_cast< unsigned char >( std::stoul( s, nullptr, 16 ) );
              ++cnt;
            }
            signatures.emplace_back( sig );
          }
        };

        create_signature( signature );

        if( multisig_num > 0 )
        {
          sql = "select * from ah_get_multi_sig_in_trx( '"+ id.str() + "' )";

          if( !execute_query( sql, result ) )
            return;

          for( const auto& row : result )
          {
            FC_ASSERT( row.size() == MULTISIG_NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields", ( "db_size", row.size() )( "req_size", OP_NR_FIELDS ) );
            signature = row[ 0 ].as< std::string >();
            create_signature( signature );
          }
        }

        sql = "select * from ah_get_ops_in_trx( " + std::to_string( sql_result.block_num ) + ", " + std::to_string( sql_result.transaction_num ) + " )";

        if( !execute_query( sql, result ) )
          return;

        _time_logger.snapshot_time();

        for( const auto& row : result )
        {
          FC_ASSERT( row.size() == OP_NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields", ( "db_size", row.size() )( "req_size", OP_NR_FIELDS ) );

          uint32_t cnt = 0;

          account_history_sql_object temp;
          temp.op   = fc::json::from_string( row[ cnt++ ].as< std::string >() );

          sql_result.operations.emplace_back( std::move( temp.get_op() ) );
        }

        sql_result.signatures = std::move( signatures );

        _time_logger.snapshot_time();
        _time_logger.info();
      }

      bool account_history_sql_plugin_impl::get_account_history_reversible( account_history_sql::account_history_sql_plugin::sql_sequence_result_type sql_sequence_result,
                                                            const hive::protocol::account_name_type& account, uint64_t start, uint32_t limit,
                                                            const fc::optional<bool>& include_reversible,
                                                            const fc::optional<uint64_t>& operation_filter_low,
                                                            const fc::optional<uint64_t>& operation_filter_high )
      {
        //Returning reversible blocks not supported therefore the call `get_account_history` always returns irreversible data.
        return false;
      }

      void account_history_sql_plugin_impl::get_account_history( account_history_sql::account_history_sql_plugin::sql_sequence_result_type sql_sequence_result,
                                                            const hive::protocol::account_name_type& account, uint64_t start, uint32_t limit,
                                                            const fc::optional<bool>& include_reversible,
                                                            const fc::optional<uint64_t>& operation_filter_low,
                                                            const fc::optional<uint64_t>& operation_filter_high )
      {
        if( get_account_history_reversible( sql_sequence_result, account, start, limit, include_reversible, operation_filter_low, operation_filter_high ) )
          return;

        std::set<uint32_t> s;
        gather_operations_from_filter( operation_filter_low, s, false/*high*/ );
        gather_operations_from_filter( operation_filter_high, s, true/*high*/ );

        std::string filter_array;
        create_filter_array( s, filter_array );

        const uint32_t NR_FIELDS = 8;

        pqxx::result result;
        std::string sql;

        time_logger _time_logger;

        sql = "select * from ah_get_account_history(" + filter_array + ", '" + account + "', " + std::to_string( start ) + ", " + std::to_string( limit ) +" ) ORDER BY _block, _trx_in_block, _op_in_trx, _virtual_op DESC";

        if( !execute_query( sql, result ) )
          return;

        _time_logger.snapshot_time();

        for( const auto& row : result )
        {
          account_history_sql_object ah_obj;

          FC_ASSERT( row.size() == NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields", ( "db_size", row.size() )( "req_size", NR_FIELDS ) );

          fill_object( row, ah_obj );

          sql_sequence_result( ah_obj.operation_id , ah_obj );
        }

        _time_logger.snapshot_time();
        _time_logger.info();
      }

      void account_history_sql_plugin_impl::enum_virtual_ops_reversible( std::vector< account_history_sql_object >& reversible_results,
                                                                        uint32_t block_range_begin, uint32_t block_range_end,
                                                                        const fc::optional<bool>& include_reversible,
                                                                        int64_t operation_begin, uint32_t limit,
                                                                        const std::set<uint32_t>& filter )
      {
        synchronize( block_range_begin, block_range_end );

        auto _filter = [ &filter, &operation_begin ]( const reversible_operation& r_op )
        {
          if( !r_op.is_virtual )
            return false;

          if( r_op.operation_id >= operation_begin )
          {
            return filter.empty() || filter.find( r_op.op_type ) != filter.end();
          }
          else
            return false;
        };

        for( uint32_t block_number = block_range_begin; block_number < block_range_end; ++block_number )
        {
          std::vector< account_history_sql_object > tmp_reversible_results;
          if( get_ops_in_block_reversible( tmp_reversible_results, block_number, _filter, limit ) )
          {
            limit -= tmp_reversible_results.size();

            for( auto& item : tmp_reversible_results )
            {
              reversible_results.emplace_back( item );
            }

            if( limit == 0 )
              break;
          }
        }

        return;
      }

      void account_history_sql_plugin_impl::enum_virtual_ops_reversible_find_next_elements( uint32_t& next_block_range_begin, uint64_t& next_operation_begin,
                                        uint32_t start_block,
                                        int64_t operation_begin,
                                        const std::set<uint32_t>& filter )
      {
        std::vector< account_history_sql_object > tmp_reversible_results;

        auto _filter = [ &filter, &operation_begin ]( const reversible_operation& r_op )
        {
          if( !r_op.is_virtual )
            return false;

          if( filter.empty() )
            return true;

          if( r_op.operation_id >= operation_begin )
            return filter.find( r_op.op_type ) != filter.end();
          else
            return false;
        };

        const auto& idx_block = _db.get_index<sql_serializer::PSQL::volatile_block_index, sql_serializer::PSQL::by_block>();
        auto itr_block = idx_block.lower_bound( start_block );
        if( itr_block == idx_block.end() )
          return;
        start_block = itr_block->get_block_number();

        while( true )
        {
          if( get_ops_in_block_reversible( tmp_reversible_results, start_block++, _filter, 1/*limit*/ ) )
          {
            if( !tmp_reversible_results.empty() )
            {
              auto& obj = tmp_reversible_results.front();

              next_block_range_begin = obj.block;
              next_operation_begin = obj.operation_id;
              return;
            }
          }
          else
            return;
        }
      }

      void account_history_sql_plugin_impl::enum_virtual_ops_irreversible_find_next_elements( uint32_t& next_block_range_begin, uint64_t& next_operation_begin,
                                        uint32_t start_block,
                                        int64_t operation_begin,
                                        const std::string& filter )
      {
        const uint32_t NR_NEXT_ELEMENTS_FIELDS = 2;

        pqxx::result result;

        std::string sql = "select * from ah_get_enum_virtual_ops_next_elements( "
                + filter + ", "
                + std::to_string( start_block ) + ", "
                + std::to_string( operation_begin ) + "::BIGINT ) ";

        if( !execute_query( sql, result ) )
          return;

        if( result.size() == 1 )
        {
          const auto& row = result[0];
          FC_ASSERT( row.size() == NR_NEXT_ELEMENTS_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields",
                                                            ( "db_size", row.size() )( "req_size", NR_NEXT_ELEMENTS_FIELDS ) );

          uint32_t cnt = 0;
          next_block_range_begin  = row[ cnt++ ].as< uint32_t >();
          next_operation_begin    = row[ cnt++ ].as< uint64_t >();
        }
      }

      void account_history_sql_plugin_impl::enum_virtual_ops( account_history_sql::account_history_sql_plugin::sql_result_type sql_result,
                                                        uint32_t block_range_begin, uint32_t block_range_end,
                                                        const fc::optional<bool>& include_reversible,
                                                        const fc::optional< uint64_t >& operation_begin, const fc::optional< uint32_t >& limit,
                                                        const fc::optional< uint32_t >& filter,
                                                        uint32_t& next_block_range_begin,
                                                        uint64_t& next_operation_begin )
      {
        next_block_range_begin  = 0;
        next_operation_begin    = 0;

        const uint32_t DEFAULT_LIMIT = 1000000;
        uint32_t _limit = limit.valid() ? *limit : DEFAULT_LIMIT;
        const int64_t _operation_begin = operation_begin.valid() ? *operation_begin : -1;

        uint32_t last_next_block_range_begin  = block_range_begin;
        uint64_t last_next_operation_begin    = _operation_begin;

        bool _reversible_is_required = reversible_is_required( include_reversible );

        std::set<uint32_t> s;
        gather_virtual_operations_from_filter( filter, s );

        const uint32_t NR_FIELDS = 8;

        pqxx::result result;
        std::string sql;

        std::string filter_array;
        create_filter_array( s, filter_array );

        time_logger _time_logger;

        sql = "select * from ah_get_enum_virtual_ops( "
                + filter_array + ", "
                + std::to_string( block_range_begin ) + ", "
                + std::to_string( block_range_end ) + ", "
                + std::to_string( _operation_begin ) + "::BIGINT, "
                + std::to_string( _limit ) + " ) ORDER BY _block, _trx_in_block, _op_in_trx";

        if( !execute_query( sql, result ) )
          return;

        _time_logger.snapshot_time();

        uint32_t result_size  = result.size();
        uint32_t cnt          = 0;
        uint32_t received     = 0;

        auto find_next_elements = [ & ]( const account_history_sql_object& ah_obj )
        {
          //The last record is received only for filling variables used for paging: `next_block_range_begin`, `next_operation_begin`
          ++cnt;

          if( cnt == result_size )
          {
            last_next_block_range_begin  = ah_obj.block;
            last_next_operation_begin    = ah_obj.operation_id;
          }
          if( cnt == _limit + 1 )
          {
            next_block_range_begin  = ah_obj.block;
            next_operation_begin    = ah_obj.operation_id;

            return true;
          }
          return false;
        };

        for( const auto& row : result )
        {
          account_history_sql_object ah_obj;

          FC_ASSERT( row.size() == NR_FIELDS, "The database returned ${db_size} fields, but there is required ${req_size} fields",
                                              ( "db_size", row.size() )( "req_size", NR_FIELDS ) );

          fill_object( row, ah_obj );

          if( find_next_elements( ah_obj ) )
            break;

          sql_result( ah_obj );
          ++received;
        }

        if( next_operation_begin == 0 )
        {
          enum_virtual_ops_irreversible_find_next_elements( next_block_range_begin, next_operation_begin, last_next_block_range_begin, last_next_operation_begin + 1, filter_array );
        }

        if( _reversible_is_required )
        {
          if( received < _limit )
          {
            _limit -= received;

            //Below there is: `_limit + 1`
            //The last record is received only for filling variables used for paging: `next_block_range_begin`, `next_operation_begin`

            std::vector< account_history_sql_object > reversible_results;

            enum_virtual_ops_reversible(  reversible_results,
                                          block_range_begin, block_range_end,
                                          include_reversible,
                                          _operation_begin, _limit + 1, s );

            result_size     = reversible_results.size();
            cnt             = 0;

            for( auto& ah_obj : reversible_results )
            {
              if( find_next_elements( ah_obj ) )
                break;

              sql_result( ah_obj );
            }
          }

          if( next_block_range_begin == 0 )
          {
            enum_virtual_ops_reversible_find_next_elements( next_block_range_begin, next_operation_begin, last_next_block_range_begin, last_next_operation_begin + 1, s );
          }
        }

        _time_logger.snapshot_time();
        _time_logger.info();
      }

    }//namespace detail
  

    account_history_sql_plugin::account_history_sql_plugin() {}
    account_history_sql_plugin::~account_history_sql_plugin() {}

    void account_history_sql_plugin::set_program_options(options_description &cli, options_description &cfg)
    {
      //ahsql-url = dbname=ah_db_name user=postgres password=pass hostaddr=127.0.0.1 port=5432
      cfg.add_options()("ahsql-url", boost::program_options::value<string>(), "postgres connection string for AH database");
    }

    void account_history_sql_plugin::plugin_initialize(const boost::program_options::variables_map &options)
    {
      ilog("Initializing SQL account history plugin");
      FC_ASSERT( options.count("ahsql-url"), "`ahsql-url` is required" );

      auto thread_pool_size = options.at("webserver-thread-pool-size").as<uint32_t>();

      my = std::make_unique<detail::account_history_sql_plugin_impl>( options["ahsql-url"].as<fc::string>(), thread_pool_size );
    }

    void account_history_sql_plugin::plugin_startup()
    {
      ilog("sql::plugin_startup()");
    }

    void account_history_sql_plugin::plugin_shutdown()
    {
      ilog("sql::plugin_shutdown()");
    }

    void account_history_sql_plugin::get_ops_in_block( sql_result_type sql_result,
                                                      uint32_t block_number, bool only_virtual, const fc::optional<bool>& include_reversible ) const
    {
      my->get_ops_in_block( sql_result, block_number, only_virtual, include_reversible );
    }

    void account_history_sql_plugin::get_transaction( hive::protocol::annotated_signed_transaction& sql_result,
                                                      const hive::protocol::transaction_id_type& id, const fc::optional<bool>& include_reversible ) const
    {
      my->get_transaction( sql_result, id, include_reversible );
    }

    void account_history_sql_plugin::get_account_history( sql_sequence_result_type sql_sequence_result,
                                                          const hive::protocol::account_name_type& account, uint64_t start, uint32_t limit,
                                                          const fc::optional<bool>& include_reversible,
                                                          const fc::optional<uint64_t>& operation_filter_low,
                                                          const fc::optional<uint64_t>& operation_filter_high ) const
    {
      my->get_account_history( sql_sequence_result, account, start, limit, include_reversible, operation_filter_low, operation_filter_high );
    }

    void account_history_sql_plugin::enum_virtual_ops( sql_result_type sql_result,
                                                      uint32_t block_range_begin, uint32_t block_range_end,
                                                      const fc::optional<bool>& include_reversible,
                                                      const fc::optional< uint64_t >& operation_begin, const fc::optional< uint32_t >& limit,
                                                      const fc::optional< uint32_t >& filter,
                                                      uint32_t& next_block_range_begin,
                                                      uint64_t& next_operation_begin ) const
    {
      my->enum_virtual_ops( sql_result, block_range_begin, block_range_end, include_reversible,
                            operation_begin, limit, filter, next_block_range_begin, next_operation_begin );
    }

  } // namespace account_history_sql
 }  // namespace plugins
} // namespace hive