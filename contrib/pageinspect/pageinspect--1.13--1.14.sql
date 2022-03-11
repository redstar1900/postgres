/* contrib/pageinspect/pageinspect--1.13--1.14.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.14'" to load this file. \quit

--
-- page_header()
--
DROP FUNCTION page_header(bytea);
CREATE FUNCTION page_header(IN page bytea,
    OUT lsn pg_lsn,
    OUT checksum smallint,
    OUT flags smallint,
    OUT lower smallint,
    OUT upper smallint,
    OUT special smallint,
    OUT pagesize smallint,
    OUT version smallint,
    OUT prune_xid xid,
    OUT xid_base xid,
    OUT multi_base xid)
AS 'MODULE_PATHNAME', 'page_header'
LANGUAGE C STRICT PARALLEL SAFE;
