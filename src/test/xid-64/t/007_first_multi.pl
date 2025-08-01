# Test for pages with first tuple has xmax multi
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub test_multixact
{
	my ($primary, $standby, $test_name) = @_;

	$primary->safe_psql('postgres', q{
		CREATE TABLE t (id INT, data TEXT, CONSTRAINT t_id_pk PRIMARY KEY(id));
		INSERT INTO t SELECT 1, repeat('a', 1000);
	});

	my %psql = (
		'1' => $primary->background_psql('postgres'),
		'2' => $primary->background_psql('postgres'),
	);

	# Lock tuples
	$psql{1}->query_safe(q(
		BEGIN;
		SELECT * FROM t FOR KEY SHARE;
	));

	$psql{2}->query_safe(q(
		BEGIN;
		SELECT * FROM t FOR KEY SHARE;
	));

	# Repeat update until we get a new page with one tuple
	my $res;
	my $guard = 0;

	do {
		$res = $primary->safe_psql('postgres', q{
			UPDATE t SET data = repeat('a', 1000) RETURNING ctid;
		});
		# Fail if we already write around 64k and still have no new page.
		fail("creating second page") if (++$guard == 64);
	} until ($res eq "(1,1)");

	$psql{1}->quit;
	$psql{2}->quit;
	$primary->wait_for_catchup($standby);

	# Check results
	my $query = q{
		SELECT xmax, ctid, id, data = repeat('a', 1000) as data FROM t;
	};
	my $res_primary = $primary->safe_psql('postgres', $query);
	my $res_standby = $standby->safe_psql('postgres', $query);

	is($res_primary, $res_standby, "rows are the same in test $test_name");
}

# We should run test for full_page_writes on and off.
foreach ('true', 'false') {
	# Create primary
	my $primary = PostgreSQL::Test::Cluster->new("master_$_");
	$primary->init(allows_streaming => 1);
	$primary->append_conf('postgresql.conf', "full_page_writes = $_");
	$primary->start;

	# Take backup
	my $backup_name = "my_backup_$_";
	$primary->backup($backup_name);

	# Create standby from backup
	my $standby = PostgreSQL::Test::Cluster->new("standby_$_");
	$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
	$standby->start;

	# Check
	test_multixact($primary, $standby, "with FPW $_");

	$standby->stop();
	$primary->stop();
}

done_testing();
