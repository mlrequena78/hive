
#include <fc/io/json.hpp>
#include <fc/smart_ref_impl.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <condition_variable>
#include <shared_mutex>
#include <thread>
#include <iostream>

#include <queue>
#include <mutex>
#include <condition_variable>

// C++ connector library for PostgreSQL (http://pqxx.org/development/libpqxx/)
#include <pqxx/pqxx>

namespace hive { namespace utilities {

    void mylog(const char* msg);
    fc::string generate(std::function<void(fc::string &)> fun);

    using postgres_connection_type = std::shared_ptr<pqxx::lazyconnection>;

    struct connection_data
    {
      postgres_connection_type conn;
      uint32_t idx;
    };

    class postgres_connection_pool
    {
      private:

        std::queue<connection_data> connections;

        std::mutex mtx;
        std::condition_variable cv;

        void init( const fc::string& connection_string, uint32_t thread_pool_size )
        {
          ilog( "SQL Connection Pool: creating `${thread_pool_size}` connections", ("thread_pool_size", thread_pool_size) );
          std::lock_guard<std::mutex> lock( mtx );

          for( uint32_t i = 0; i < thread_pool_size; ++i )
          {
            connections.emplace( connection_data{ std::make_shared< pqxx::lazyconnection >( connection_string ), i } );
          }
          ilog( "SQL Connection Pool: connections have been created" );
        }

      public:

        postgres_connection_pool( const fc::string& connection_string, uint32_t thread_pool_size )
        {
          init( connection_string, thread_pool_size );
        }

        connection_data get_connection()
        {
          std::unique_lock<std::mutex> lock( mtx );

          //Formally a situation where all threads would be used, it's impossible
          //because size of pool is dependent on `webserver-thread-pool-size`,
          //so http layer doesn't allow to execute more threads than `webserver-thread-pool-size` value 
          while( connections.empty() )
          {
              cv.wait( lock );
          }

          auto conn = connections.front();
          connections.pop();

          ilog( "SQL Connection Pool: connection `${idx}` has been chosen", ("idx", conn.idx) );
          return conn;
        }

        void restore_connection( connection_data conn )
        {
          std::unique_lock<std::mutex> lock( mtx );

          connections.emplace( std::move( conn ) );

          lock.unlock();

          cv.notify_one();
          ilog( "SQL Connection Pool: connection `${idx}` has been restored", ("idx", conn.idx) );
        }
    };

    struct transaction_repr_t
    {
     std::unique_ptr<pqxx::connection> _connection;
     std::unique_ptr<pqxx::work> _transaction;

     transaction_repr_t() = default;
     transaction_repr_t(pqxx::connection* _conn, pqxx::work* _trx) : _connection{_conn}, _transaction{_trx} {}

     auto get_escaping_charachter_methode() const
     {
      return [this](const char *val) -> fc::string { return std::move( this->_connection->esc(val) ); };
     }

     auto get_raw_escaping_charachter_methode() const
     {
      return [this](const char *val, const size_t s) -> fc::string { 
       pqxx::binarystring __tmp(val, s); 
       return std::move( this->_transaction->esc_raw( __tmp.str() ) ); 
      };
     }
    };

    using transaction_t = std::unique_ptr<transaction_repr_t>;

    struct postgres_connection_holder
    {
      explicit postgres_connection_holder(const fc::string &url)
       : connection_string{url} {}

      transaction_t start_transaction() const
      {
        // mylog("started transaction");
        pqxx::connection* _conn = new pqxx::connection{ this->connection_string };
        pqxx::work *_trx = new pqxx::work{*_conn};
        _trx->exec("SET CONSTRAINTS ALL DEFERRED;");

        return transaction_t{ new transaction_repr_t{ _conn, _trx } };
      }

      bool exec_transaction(transaction_t &trx, const fc::string &sql) const
      {
        if (sql == fc::string())
          return true;
        return sql_safe_execution([&trx, &sql]() { trx->_transaction->exec(sql); }, sql.c_str());
      }

      bool commit_transaction(transaction_t &trx) const
      {
        // mylog("commiting");
        return sql_safe_execution([&]() { trx->_transaction->commit(); }, "commit");
      }

      void abort_transaction(transaction_t &trx) const
      {
        // mylog("aborting");
        trx->_transaction->abort();
      }

      bool exec_single_in_transaction(const fc::string &sql, pqxx::result *result = nullptr) const
      {
        if (sql == fc::string())
          return true;

        return sql_safe_execution([&]() {
        pqxx::connection conn{ this->connection_string };
        pqxx::work trx{conn};
        if (result)
          *result = trx.exec(sql);
        else
          trx.exec(sql);
        trx.commit();
        }, sql.c_str());
      }

      bool exec_query( pqxx::lazyconnection& conn, const fc::string &sql, pqxx::result *result = nullptr ) const
      {
        if (sql == fc::string())
          return true;

        return sql_safe_execution( [&]() {
          pqxx::read_transaction trx{ conn };
          if( result )
            *result = trx.exec( sql );
          else
            trx.exec( sql );
          trx.commit();
          },
          sql.c_str()
        );
      }

      template<typename T>
      bool get_single_value(const fc::string& query, T& _return) const
      {
        pqxx::result res;
        if(!exec_single_in_transaction( query, &res ) && res.empty() ) return false;
          _return = res.at(0).at(0).as<T>();
        return true;
      }

      template<typename T>
      T get_single_value(const fc::string& query) const
      {
        T _return;
        FC_ASSERT( get_single_value( query, _return ) );
        return _return;
      }

      uint get_max_transaction_count() const
      {
        // get maximum amount of connections defined by postgres and set half of it as max; minimum is 1
        return std::max( 1u, get_single_value<uint>("SELECT setting::int / 2 FROM pg_settings WHERE  name = 'max_connections'") );
      }

      private:

      fc::string connection_string;

      bool sql_safe_execution(const std::function<void()> &f, const char* msg = nullptr) const
      {
        try
        {
          f();
          return true;
        }
        catch (const pqxx::pqxx_exception &sql_e)
        {
          elog("Exception from postgress: ${e}", ("e", sql_e.base().what()));
        }
        catch (const std::exception &e)
        {
          elog("Exception: ${e}", ("e", e.what()));
        }
        catch (...)
        {
          elog("Unknown exception occured");
        }

        if(msg)
          std::cerr << "Error message: " << msg << std::endl;

        return false;
      }
    };

} }