DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
CREATE EXTENSION IF NOT EXISTS intarray;

CREATE TABLE IF NOT EXISTS hive_blocks (
  "num" integer NOT NULL,
  "hash" bytea NOT NULL,
  "prev" bytea NOT NULL,
  "created_at" timestamp without time zone NOT NULL
);
ALTER TABLE hive_blocks ADD CONSTRAINT hive_blocks_pkey PRIMARY KEY ( num );
ALTER TABLE hive_blocks ADD CONSTRAINT hive_blocks_uniq UNIQUE ( hash );

CREATE TABLE IF NOT EXISTS hive_transactions (
  "block_num" integer NOT NULL,
  "trx_in_block" smallint NOT NULL,
  "trx_hash" bytea NOT NULL,
  ref_block_num integer NOT NULL,
  ref_block_prefix bigint NOT NULL,
  expiration timestamp without time zone NOT NULL,
  signature bytea DEFAULT NULL
);
ALTER TABLE hive_transactions ADD CONSTRAINT hive_transactions_pkey PRIMARY KEY ( trx_hash );
CREATE INDEX IF NOT EXISTS hive_transactions_block_num_trx_in_block_idx ON hive_transactions ( block_num, trx_in_block );

CREATE TABLE IF NOT EXISTS hive_transactions_multisig (
  "trx_hash" bytea NOT NULL,
  signature bytea NOT NULL
);
ALTER TABLE hive_transactions_multisig ADD CONSTRAINT hive_transactions_multisig_pkey PRIMARY KEY ( trx_hash, signature );

CREATE TABLE IF NOT EXISTS hive_operation_types (
  "id" smallint NOT NULL,
  "name" text NOT NULL,
  "is_virtual" boolean NOT NULL,
  CONSTRAINT hive_operation_types_pkey PRIMARY KEY (id),
  CONSTRAINT hive_operation_types_uniq UNIQUE (name)
);

CREATE TABLE IF NOT EXISTS hive_permlink_data (
  id INTEGER NOT NULL,
  permlink varchar(255) NOT NULL
);
ALTER TABLE hive_permlink_data ADD CONSTRAINT hive_permlink_data_pkey PRIMARY KEY ( id );
ALTER TABLE hive_permlink_data ADD CONSTRAINT hive_permlink_data_uniq UNIQUE ( permlink );

CREATE TABLE IF NOT EXISTS hive_operations (
  id bigint not null,
  block_num integer NOT NULL,
  trx_in_block smallint NOT NULL,
  op_pos integer NOT NULL,
  op_type_id smallint NOT NULL,
  body text DEFAULT NULL
);
ALTER TABLE hive_operations ADD CONSTRAINT hive_operations_pkey PRIMARY KEY ( id );
CREATE INDEX IF NOT EXISTS hive_operations_block_num_type_trx_in_block_idx ON hive_operations ( block_num, op_type_id, trx_in_block );

CREATE TABLE IF NOT EXISTS hive_accounts (
  id INTEGER NOT NULL,
  name VARCHAR(16) NOT NULL
);
ALTER TABLE hive_accounts ADD CONSTRAINT hive_accounts_pkey PRIMARY KEY ( id );
ALTER TABLE hive_accounts ADD CONSTRAINT hive_accounts_uniq UNIQUE ( name );

CREATE TABLE IF NOT EXISTS hive_account_operations
(
  --- Identifier of account involved in given operation.
  account_id integer not null,
  --- Operation sequence number specific to given account. 
  account_op_seq_no integer not null,
  --- Id of operation held in hive_opreations table.
  operation_id bigint not null
);
CREATE INDEX IF NOT EXISTS hive_account_operations_account_op_seq_no_id_idx ON hive_account_operations(account_id, account_op_seq_no, operation_id);
CREATE INDEX IF NOT EXISTS hive_account_operations_operation_id_idx ON hive_account_operations (operation_id);

ALTER TABLE hive_account_operations ADD CONSTRAINT hive_account_operations_fk_1 FOREIGN KEY (account_id) REFERENCES hive_accounts (id);
ALTER TABLE hive_account_operations ADD CONSTRAINT hive_account_operations_fk_2 FOREIGN KEY (operation_id) REFERENCES hive_operations (id);
ALTER TABLE hive_operations ADD CONSTRAINT hive_operations_fk_1 FOREIGN KEY (block_num) REFERENCES hive_blocks (num);
ALTER TABLE hive_operations ADD CONSTRAINT hive_operations_fk_2 FOREIGN KEY (op_type_id) REFERENCES hive_operation_types (id);
ALTER TABLE hive_transactions ADD CONSTRAINT hive_transactions_fk_1 FOREIGN KEY (block_num) REFERENCES hive_blocks (num);
ALTER TABLE hive_transactions_multisig ADD CONSTRAINT hive_transactions_multisig_fk_1 FOREIGN KEY (trx_hash) REFERENCES hive_transactions (trx_hash);

DROP VIEW IF EXISTS account_operation_count_info_view CASCADE;
CREATE OR REPLACE VIEW account_operation_count_info_view
AS
SELECT ha.id, ha.name, COALESCE( T.operation_count, 0 ) operation_count
FROM hive_accounts ha
LEFT JOIN
(
SELECT ao.account_id account_id, COUNT(ao.account_op_seq_no) operation_count
FROM hive_account_operations ao
GROUP BY ao.account_id
)T ON ha.id = T.account_id
;

INSERT INTO hive_permlink_data VALUES(0, '');
INSERT INTO hive_accounts VALUES(0, '');

DROP TYPE IF EXISTS hive_api_operation CASCADE;
CREATE TYPE hive_api_operation AS (
    id BIGINT,
    block_num INT,
    operation_type_id SMALLINT,
    is_virtual BOOLEAN,
    body VARCHAR
);

CREATE OR REPLACE FUNCTION enum_operations4hivemind(in _first_block INT, in _last_block INT)
RETURNS SETOF hive_api_operation
AS
$function$
BEGIN
  RETURN QUERY -- enum_operations4hivemind
    SELECT ho.id, ho.block_num, ho.op_type_id, ho.op_type_id >= 48 AS is_virtual, ho.body::VARCHAR
    FROM hive_operations ho
    WHERE ho.block_num between _first_block and _last_block
          AND (ho.op_type_id < 48 -- (select t.id from hive_operation_types t where t.is_virtual order by t.id limit 1)
               OR ho.op_type_id in (49, 51, 59, 70, 71)
              )
    ORDER BY ho.id
; 

END
$function$
LANGUAGE plpgsql STABLE
;

DROP TYPE IF EXISTS hive_api_hivemind_blocks CASCADE;
CREATE TYPE hive_api_hivemind_blocks AS (
    num INTEGER,
    hash BYTEA,
    prev BYTEA,
    date TEXT,
    tx_number BIGINT,
    op_number BIGINT
    );


CREATE OR REPLACE FUNCTION enum_blocks4hivemind(in _first_block INT, in _last_block INT)
RETURNS SETOF hive_api_hivemind_blocks
AS
$function$
BEGIN
RETURN QUERY
SELECT
    hb.num
     , hb.hash
     , hb.prev as prev
     , to_char( created_at,  'YYYY-MM-DDThh24:MI:SS' ) as date
        , ( SELECT COUNT(*) tx_number  FROM hive_transactions ht WHERE ht.block_num = hb.num ) as tx_number
        , ( SELECT COUNT(*) op_number  FROM hive_operations ho WHERE ho.block_num = hb.num AND ( ho.op_type_id < 48 OR ho.op_type_id in (49, 51, 59, 70, 71) ) ) as op_number
FROM hive_blocks hb
WHERE hb.num >= _first_block AND hb.num < _last_block
ORDER by hb.num
;
END
$function$
LANGUAGE plpgsql STABLE
;

DROP TABLE IF EXISTS hive_indexes_constraints;
CREATE TABLE IF NOT EXISTS hive_indexes_constraints (
  "table_name" text NOT NULL,
  "index_constraint_name" text NOT NULL,
  "command" text NOT NULL,
  "is_constraint" boolean NOT NULL,
  "is_index" boolean NOT NULL,
  "is_foreign_key" boolean NOT NULL
);
ALTER TABLE hive_indexes_constraints ADD CONSTRAINT hive_indexes_constraints_table_name_idx UNIQUE ( table_name, index_constraint_name );

DROP FUNCTION IF EXISTS save_and_drop_indexes_constraints;
CREATE OR REPLACE FUNCTION save_and_drop_indexes_constraints( in _table_name TEXT )
RETURNS VOID
AS
$function$
DECLARE
  __command TEXT;
  __cursor REFCURSOR;
BEGIN

  INSERT INTO hive_indexes_constraints( table_name, index_constraint_name, command, is_constraint, is_index, is_foreign_key )
  SELECT
    T.table_name,
    T.constraint_name,
    (
      CASE
        WHEN T.is_primary = TRUE THEN 'ALTER TABLE ' || T.table_name || ' ADD CONSTRAINT ' || T.constraint_name || ' PRIMARY KEY ( ' || array_to_string(array_agg( T.column_name::TEXT ), ', ') || ' ) '
        WHEN (T.is_unique = TRUE AND T.is_primary = FALSE ) THEN 'ALTER TABLE ' || T.table_name || ' ADD CONSTRAINT ' || T.constraint_name || ' UNIQUE ( ' || array_to_string(array_agg( T.column_name::TEXT ), ', ') || ' ) '
        WHEN (T.is_unique = FALSE AND T.is_primary = FALSE ) THEN 'CREATE INDEX IF NOT EXISTS ' || T.constraint_name || ' ON ' || T.table_name || ' ( ' || array_to_string(array_agg( T.column_name::TEXT ), ', ') || ' ) '
      END
    ),
    (T.is_unique = TRUE OR T.is_primary = TRUE ) is_constraint,
    (T.is_unique = FALSE AND T.is_primary = FALSE ) is_index,
    FALSE is_foreign_key
  FROM
  (
    SELECT
        t.relname table_name,
        i.relname constraint_name,
        a.attname column_name,
        ix.indisunique is_unique,
        ix.indisprimary is_primary,
        a.attnum
    FROM
        pg_class t,
        pg_class i,
        pg_index ix,
        pg_attribute a
    WHERE
        t.oid = ix.indrelid
        AND i.oid = ix.indexrelid
        AND a.attrelid = t.oid
        AND a.attnum = ANY(ix.indkey)
        AND t.relkind = 'r'
        AND t.relname = _table_name
    ORDER BY t.relname, i.relname, a.attnum
  )T
  GROUP BY T.table_name, T.constraint_name, T.is_unique, T.is_primary
  ON CONFLICT DO NOTHING;

  --dropping indexes
  OPEN __cursor FOR ( SELECT ('DROP INDEX IF EXISTS '::TEXT || index_constraint_name || ';') FROM hive_indexes_constraints WHERE table_name = _table_name AND is_index = TRUE );
  LOOP
    FETCH __cursor INTO __command;
    EXIT WHEN NOT FOUND;
    EXECUTE __command;
  END LOOP;
  CLOSE __cursor;

  --dropping primary keys/unique contraints
  OPEN __cursor FOR ( SELECT ('ALTER TABLE '::TEXT || _table_name || ' DROP CONSTRAINT IF EXISTS ' || index_constraint_name || ';') FROM hive_indexes_constraints WHERE table_name = _table_name AND is_constraint = TRUE );
  LOOP
    FETCH __cursor INTO __command;
    EXIT WHEN NOT FOUND;
    EXECUTE __command;
  END LOOP;
  CLOSE __cursor;

END
$function$
LANGUAGE plpgsql VOLATILE
;

DROP FUNCTION IF EXISTS save_and_drop_indexes_foreign_keys;
CREATE OR REPLACE FUNCTION save_and_drop_indexes_foreign_keys( in _table_name TEXT )
RETURNS VOID
AS
$function$
DECLARE
  __command TEXT;
  __cursor REFCURSOR;
BEGIN

  INSERT INTO hive_indexes_constraints( table_name, index_constraint_name, command, is_constraint, is_index, is_foreign_key )
  SELECT
    tc.table_name,
    tc.constraint_name,
    'ALTER TABLE ' || tc.table_name || ' ADD CONSTRAINT ' || tc.constraint_name || ' FOREIGN KEY ( ' || kcu.column_name || ' ) REFERENCES ' || ccu.table_name || ' ( ' || ccu.column_name || ' ) ',
    FALSE is_constraint,
    FALSE is_index,
    TRUE is_foreign_key
  FROM 
      information_schema.table_constraints AS tc 
      JOIN information_schema.key_column_usage AS kcu ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema
      JOIN information_schema.constraint_column_usage AS ccu ON ccu.constraint_name = tc.constraint_name AND ccu.table_schema = tc.table_schema
  WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_name = _table_name
  ON CONFLICT DO NOTHING;

  OPEN __cursor FOR ( SELECT ('ALTER TABLE '::TEXT || _table_name || ' DROP CONSTRAINT IF EXISTS ' || index_constraint_name || ';') FROM hive_indexes_constraints WHERE table_name = _table_name AND is_foreign_key = TRUE );

  LOOP
    FETCH __cursor INTO __command;
    EXIT WHEN NOT FOUND;
    EXECUTE __command;
  END LOOP;

  CLOSE __cursor;

END
$function$
LANGUAGE plpgsql VOLATILE
;

DROP FUNCTION IF EXISTS restore_indexes_constraints;
CREATE OR REPLACE FUNCTION restore_indexes_constraints( in _table_name TEXT )
RETURNS VOID
AS
$function$
DECLARE
  __command TEXT;
  __cursor REFCURSOR;
BEGIN

  --restoring indexes, primary keys, unique contraints
  OPEN __cursor FOR ( SELECT command FROM hive_indexes_constraints WHERE table_name = _table_name AND is_foreign_key = FALSE );
  LOOP
    FETCH __cursor INTO __command;
    EXIT WHEN NOT FOUND;
    EXECUTE __command;
  END LOOP;
  CLOSE __cursor;

  DELETE FROM hive_indexes_constraints
  WHERE table_name = _table_name AND is_foreign_key = FALSE;

END
$function$
LANGUAGE plpgsql VOLATILE
;

DROP FUNCTION IF EXISTS restore_foreign_keys;
CREATE OR REPLACE FUNCTION restore_foreign_keys( in _table_name TEXT )
RETURNS VOID
AS
$function$
DECLARE
  __command TEXT;
  __cursor REFCURSOR;
BEGIN

  --restoring indexes, primary keys, unique contraints
  OPEN __cursor FOR ( SELECT command FROM hive_indexes_constraints WHERE table_name = _table_name AND is_foreign_key = TRUE );
  LOOP
    FETCH __cursor INTO __command;
    EXIT WHEN NOT FOUND;
    EXECUTE __command;
  END LOOP;
  CLOSE __cursor;

  DELETE FROM hive_indexes_constraints
  WHERE table_name = _table_name AND is_foreign_key = TRUE;

END
$function$
LANGUAGE plpgsql VOLATILE
;

DROP FUNCTION IF EXISTS ah_get_ops_in_block;
CREATE FUNCTION ah_get_ops_in_block( in _BLOCK_NUM INT, in _ONLY_VIRTUAL BOOLEAN )
RETURNS TABLE(
    _trx_id TEXT,
    _trx_in_block BIGINT,
    _op_in_trx BIGINT,
    _virtual_op BOOLEAN,
    _timestamp TEXT,
    _value TEXT,
    _operation_id INT
)
AS
$function$
BEGIN

  RETURN QUERY
    SELECT
      (
        CASE
        WHEN ht.trx_hash IS NULL THEN '0000000000000000000000000000000000000000'
        ELSE encode( ht.trx_hash, 'escape')
        END
      ) _trx_id,
      (
        CASE
        WHEN ht.trx_in_block IS NULL THEN 4294967295
        ELSE ht.trx_in_block
        END
      ) _trx_in_block,
      T.op_pos _op_in_trx,
      T.is_virtual _virtual_op,
      trim(both '"' from to_json(hb.created_at)::text) _timestamp,
      T.body _value,
      T.id::INT _operation_id
    FROM
      (
        --`abs` it's temporary, until position of operation is correctly saved
        SELECT
          ho.id, ho.block_num, ho.trx_in_block, abs(ho.op_pos::BIGINT) op_pos, ho.body, ho.op_type_id, hot.is_virtual
        FROM hive_operations ho
        JOIN hive_operation_types hot ON hot.id = ho.op_type_id
        WHERE ho.block_num = _BLOCK_NUM AND ( _ONLY_VIRTUAL = FALSE OR ( _ONLY_VIRTUAL = TRUE AND hot.is_virtual = TRUE ) )
      ) T
      JOIN hive_blocks hb ON hb.num = T.block_num
      LEFT JOIN hive_transactions ht ON T.block_num = ht.block_num AND T.trx_in_block = ht.trx_in_block;
END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_trx;
CREATE FUNCTION ah_get_trx( in _TRX_HASH BYTEA )
RETURNS TABLE(
    _ref_block_num INT,
    _ref_block_prefix BIGINT,
    _expiration TEXT,
    _block_num INT,
    _trx_in_block SMALLINT,
    _signature TEXT,
    _multisig_number SMALLINT
)
AS
$function$
DECLARE
  __multisig_number SMALLINT;
BEGIN

  SELECT count(*) INTO __multisig_number FROM hive_transactions_multisig htm WHERE htm.trx_hash = _TRX_HASH;

  RETURN QUERY
    SELECT
      ref_block_num _ref_block_num,
      ref_block_prefix _ref_block_prefix,
      '2016-06-20T19:34:09' _expiration,--lack of data
      ht.block_num _block_num,
      ht.trx_in_block _trx_in_block,
      encode(ht.signature, 'escape') _signature,
      __multisig_number
    FROM hive_transactions ht
    WHERE ht.trx_hash = _TRX_HASH;
END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_multi_sig_in_trx;
CREATE FUNCTION ah_get_multi_sig_in_trx( in _TRX_HASH BYTEA )
RETURNS TABLE(
    _signature TEXT
)
AS
$function$
BEGIN

  RETURN QUERY
    SELECT
      encode(htm.signature, 'escape') _signature
    FROM hive_transactions_multisig htm
    WHERE htm.trx_hash = _TRX_HASH;
END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_ops_in_trx;
CREATE FUNCTION ah_get_ops_in_trx( in _BLOCK_NUM INT, in _TRX_IN_BLOCK INT )
RETURNS TABLE(
    _value TEXT
)
AS
$function$
BEGIN
  RETURN QUERY
    SELECT
      ho.body _value
    FROM hive_operations ho
    JOIN hive_operation_types hot ON ho.op_type_id = hot.id
    WHERE ho.block_num = _BLOCK_NUM AND ho.trx_in_block = _TRX_IN_BLOCK AND hot.is_virtual = FALSE
    ORDER BY ho.id;
END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_account_history;
CREATE FUNCTION ah_get_account_history( in _FILTER INT[], in _ACCOUNT VARCHAR, _START INT, _LIMIT INT )
RETURNS TABLE(
    _trx_id TEXT,
    _block INT,
    _trx_in_block BIGINT,
    _op_in_trx BIGINT,
    _virtual_op BOOLEAN,
    _timestamp TEXT,
    _value TEXT,
    _operation_id INT
)
AS
$function$
DECLARE
  __account_id INT;
  __filter_info INT;
BEGIN

  SELECT INTO __filter_info ( select array_length( _FILTER, 1 ) );

  SELECT INTO __account_id ( select id from hive_accounts where name = _ACCOUNT );

  IF __filter_info IS NULL THEN
  RETURN QUERY
    SELECT
      (
        CASE
        WHEN ht.trx_hash IS NULL THEN '0000000000000000000000000000000000000000'
        ELSE encode( ht.trx_hash, 'escape')
        END
      ) _trx_id,
      ho.block_num _block,
      (
        CASE
        WHEN ht.trx_in_block IS NULL THEN 4294967295
        ELSE ht.trx_in_block
        END
      ) _trx_in_block,
      abs(ho.op_pos::BIGINT) AS _op_in_trx,
      hot.is_virtual _virtual_op,
      trim(both '"' from to_json(hb.created_at)::text) _timestamp,
      ho.body _value,
      T.seq_no AS _operation_id
      FROM
      (
        SELECT hao.operation_id as operation_id, hao.account_op_seq_no as seq_no
        FROM hive_account_operations hao 
        WHERE hao.account_id = __account_id AND hao.account_op_seq_no <= _START
        ORDER BY seq_no DESC
        LIMIT _LIMIT
      ) T
    JOIN hive_operations ho ON T.operation_id = ho.id
    JOIN hive_blocks hb ON hb.num = ho.block_num
    JOIN hive_operation_types hot ON hot.id = ho.op_type_id
    LEFT JOIN hive_transactions ht ON ho.block_num = ht.block_num AND ho.trx_in_block = ht.trx_in_block
    LIMIT _LIMIT;
  ELSE
    RETURN QUERY
      SELECT
        (
          CASE
          WHEN ht.trx_hash IS NULL THEN '0000000000000000000000000000000000000000'
          ELSE encode( ht.trx_hash, 'escape')
          END
        ) _trx_id,
        T.block_num _block,
        (
          CASE
          WHEN ht.trx_in_block IS NULL THEN 4294967295
          ELSE ht.trx_in_block
          END
        ) _trx_in_block,
        T.op_pos _op_in_trx,
        hot.is_virtual _virtual_op,
        trim(both '"' from to_json(hb.created_at)::text) _timestamp,
        T.body _value,
        T.seq_no as _operation_id
      FROM
        (
          --`abs` it's temporary, until position of operation is correctly saved
          SELECT
            ho.id, ho.block_num, ho.trx_in_block, abs(ho.op_pos::BIGINT) op_pos, ho.body, ho.op_type_id, hao.account_op_seq_no as seq_no
            FROM hive_operations ho
            JOIN hive_account_operations hao ON ho.id = hao.operation_id
            WHERE hao.account_id = __account_id AND hao.account_op_seq_no <= _START
            AND ( ho.op_type_id = ANY( _FILTER ) ) 
            ORDER BY seq_no DESC
            LIMIT _LIMIT
        ) T
        JOIN hive_blocks hb ON hb.num = T.block_num
        JOIN hive_operation_types hot ON hot.id = T.op_type_id
        LEFT JOIN hive_transactions ht ON T.block_num = ht.block_num AND T.trx_in_block = ht.trx_in_block
      LIMIT _LIMIT;

  END IF;

END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_enum_virtual_ops;
CREATE FUNCTION ah_get_enum_virtual_ops( in _FILTER INT[], in _BLOCK_RANGE_BEGIN INT, in _BLOCK_RANGE_END INT, _OPERATION_BEGIN BIGINT, in _LIMIT INT )
RETURNS TABLE(
    _trx_id TEXT,
    _block INT,
    _trx_in_block BIGINT,
    _op_in_trx BIGINT,
    _virtual_op BOOLEAN,
    _timestamp TEXT,
    _value TEXT,
    _operation_id INT
)
AS
$function$
DECLARE
  __filter_info INT;
BEGIN

   SELECT INTO __filter_info ( select array_length( _FILTER, 1 ) );

  RETURN QUERY
    SELECT
      (
        CASE
        WHEN ht.trx_hash IS NULL THEN '0000000000000000000000000000000000000000'
        ELSE encode( ht.trx_hash, 'escape')
        END
      ) _trx_id,
      T.block_num _block,
      (
        CASE
        WHEN ht.trx_in_block IS NULL THEN 4294967295
        ELSE ht.trx_in_block
        END
      ) _trx_in_block,
      T.op_pos _op_in_trx,
      TRUE _virtual_op,
      trim(both '"' from to_json(hb.created_at)::text) _timestamp,
      T.body _value,
      T.id::INT _operation_id
    FROM
      (
        --`abs` it's temporary, until position of operation is correctly saved
        SELECT
          ho.id, ho.block_num, ho.trx_in_block, abs(ho.op_pos::BIGINT) op_pos, ho.body, ho.op_type_id
          FROM hive_operations ho
          JOIN hive_operation_types hot ON hot.id = ho.op_type_id
          WHERE ho.block_num >= _BLOCK_RANGE_BEGIN AND ho.block_num < _BLOCK_RANGE_END
          AND hot.is_virtual = TRUE
          AND ( ( __filter_info IS NULL ) OR ( ho.op_type_id = ANY( _FILTER ) ) )
          AND ( _OPERATION_BEGIN = -1 OR ho.id >= _OPERATION_BEGIN )
          --There is `+1` because next block/operation is needed in order to do correct paging. This won't be put into result set.
          ORDER BY ho.id
          LIMIT _LIMIT + 1
      ) T
      JOIN hive_blocks hb ON hb.num = T.block_num
      LEFT JOIN hive_transactions ht ON T.block_num = ht.block_num AND T.trx_in_block = ht.trx_in_block;
END
$function$
language plpgsql STABLE;

DROP FUNCTION IF EXISTS ah_get_enum_virtual_ops_next_elements;
CREATE FUNCTION ah_get_enum_virtual_ops_next_elements( in _FILTER INT[], in _START_BLOCK INT, _OPERATION_BEGIN BIGINT )
RETURNS TABLE(
    _block INT,
    _operation_id INT
)
AS
$function$
DECLARE
  __filter_info INT;
BEGIN

   SELECT INTO __filter_info ( select array_length( _FILTER, 1 ) );

  RETURN QUERY
        SELECT
          ho.block_num _block,
          ho.id::INT _operation_id
          FROM hive_operations ho
          JOIN hive_operation_types hot ON hot.id = ho.op_type_id
          WHERE ho.block_num >= _START_BLOCK
          AND hot.is_virtual = TRUE
          AND ( ( __filter_info IS NULL ) OR ( ho.op_type_id = ANY( _FILTER ) ) )
          AND ( _OPERATION_BEGIN = -1 OR ho.id >= _OPERATION_BEGIN )
          ORDER BY ho.id
          LIMIT 1;
END
$function$
language plpgsql STABLE;