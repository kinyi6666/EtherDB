-- EtherDB schema import example
# A '#' is also treated as a comment by the source importer.

CREATE DATABASE IF NOT EXISTS testdb KEEP 3650 REPLICA 1;
USE testdb;

/* Multi-line CREATE TABLE statement is supported: */
CREATE TABLE weather (
    ts        TIMESTAMP,
    city      NCHAR(32),
    temp      FLOAT,
    humidity  INT
);

CREATE TABLE IF NOT EXISTS sensor_1 (ts TIMESTAMP, value FLOAT);
