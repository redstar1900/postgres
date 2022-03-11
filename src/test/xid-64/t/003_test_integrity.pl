# Check integrity after dump/restore with different xids
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Compare;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
use bigint;

my $START_VAL = 2**32;
my $MAX_VAL = 2**62;

my $ixid = $START_VAL + int(rand($MAX_VAL - $START_VAL));
my $imxid = $START_VAL + int(rand($MAX_VAL - $START_VAL));
my $imoff = $START_VAL + int(rand($MAX_VAL - $START_VAL));

# Initialize master node
my $node = PostgreSQL::Test::Cluster->new('master');
$node->init;
$node->append_conf('postgresql.conf', "log_statement = none");
$node->start;

# Create a database and fill it with the pgbench data
$node->command_ok(
	[ qw(pgbench --initialize --scale=1 --unlogged-tables postgres) ],
	  'pgbench finished without errors');
# Delete some
$node->safe_psql('postgres', qq(
	--
	DELETE FROM pgbench_tellers WHERE tid IN (
	SELECT tid FROM pgbench_tellers TABLESAMPLE BERNOULLI (50));
	--
	DELETE FROM pgbench_accounts WHERE aid IN (
	SELECT aid FROM pgbench_accounts TABLESAMPLE BERNOULLI (70));
	));
# Dump the database (cluster the main table to put data in a determined order)
$node->safe_psql('postgres', qq(
	CREATE INDEX pa_aid_idx ON pgbench_accounts (aid);
	CLUSTER pgbench_accounts USING pa_aid_idx));
$node->command_ok(
	[ "pg_dump", "-w", "--inserts", "--no-statistics",
	  "--file=$tempdir/pgbench.sql", "postgres" ],
	  'pgdump finished without errors');
$node->stop;

# Initialize second node
my $node2 = PostgreSQL::Test::Cluster->new('master2');
$node2->init(extra => [ "--xid=$ixid", "--multixact-id=$imxid",
						"--multixact-offset=$imoff" ]);
# Disable logging of all statements to avoid log bloat during restore
$node2->append_conf('postgresql.conf', "log_statement = none");
$node2->start;

# Create a database and restore the previous dump
my $txid0 = $node2->safe_psql('postgres', 'SELECT txid_current()');
print "# Initial txid_current: $txid0\n";
print "# Temp SQL file is: $tempdir/pgbench.sql\n";

$node2->command_ok(["psql", "-q", "-f", "$tempdir/pgbench.sql", "postgres"]);

# Dump the database and compare the dumped content with the previous one
$node2->safe_psql('postgres', 'CLUSTER pgbench_accounts');
$node2->command_ok(
	[ "pg_dump", "-w", "--inserts", "--no-statistics",
	  "--file=$tempdir/pgbench2.sql", "postgres" ],
	  'pgdump finished without errors');
$node2->stop;

ok(File::Compare::compare_text("$tempdir/pgbench.sql", "$tempdir/pgbench2.sql") == 0, "no differences detected");

done_testing();
