#
# This include file is used by more than one test suite
# (currently binlog and binlog_encryption).
# Please check all dependent tests after modifying it
#

--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/have_binlog_format_row.inc
# Valgrind does not work well with test that crashes the server
--source include/not_valgrind.inc

# (We do not need to restore these settings, as we crash the server).
SET GLOBAL max_binlog_size= 4096;
SET GLOBAL innodb_flush_log_at_trx_commit= 1;
RESET MASTER;

CREATE TABLE t1 (a INT PRIMARY KEY, b MEDIUMTEXT) ENGINE=Innodb;
# Insert some data to force a couple binlog rotations (3), so we get some
# normal binlog checkpoints before starting the test.
INSERT INTO t1 VALUES (100, REPEAT("x", 4100));
# Wait for the master-bin.000002 binlog checkpoint to appear.
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000002"
--let $field= Info
--let $condition= = "master-bin.000002"
--source include/wait_show_condition.inc
INSERT INTO t1 VALUES (101, REPEAT("x", 4100));
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000003"
--let $field= Info
--let $condition= = "master-bin.000003"
--source include/wait_show_condition.inc
INSERT INTO t1 VALUES (102, REPEAT("x", 4100));
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000004"
--let $field= Info
--let $condition= = "master-bin.000004"
--source include/wait_show_condition.inc

# Now start a bunch of transactions that span multiple binlog
# files. Leave then in the state prepared-but-not-committed in the engine
# and crash the server. Check that crash recovery is able to recover all
# of them.
#
# We use debug_sync to get all the transactions into the prepared state before
# we commit any of them. This is because the prepare step flushes the InnoDB
# redo log - including any commits made before, so recovery would become
# unnecessary, decreasing the value of this test.
#
# We arrange to have con1 with a prepared transaction in master-bin.000004,
# con2 and con3 with a prepared transaction in master-bin.000005, and a new
# empty master-bin.000006. So the latest binlog checkpoint should be
# master-bin.000006.

connect(con1,localhost,root,,);
# First wait after prepare and before write to binlog.
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL con1_wait WAIT_FOR con1_cont";
# Then complete InnoDB commit in memory (but not commit checkpoint / write to
# disk), and hang until crash, leaving a transaction to be XA recovered.
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con1_ready WAIT_FOR _ever";
send INSERT INTO t1 VALUES (1, REPEAT("x", 4100));

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con1_wait";

connect(con2,localhost,root,,);
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL con2_wait WAIT_FOR con2_cont";
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con2_ready WAIT_FOR _ever";
send INSERT INTO t1 VALUES (2, NULL);

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con2_wait";

connect(con3,localhost,root,,);
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL con3_wait WAIT_FOR con3_cont";
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con3_ready WAIT_FOR _ever";
send INSERT INTO t1 VALUES (3, REPEAT("x", 4100));

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con3_wait";

connect(con4,localhost,root,,);
SET DEBUG_SYNC= "ha_commit_trans_before_log_and_order SIGNAL con4_wait WAIT_FOR con4_cont";
SET SESSION debug_dbug="+d,crash_commit_after_log";
send INSERT INTO t1 VALUES (4, NULL);

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con4_wait";

SET DEBUG_SYNC= "now SIGNAL con1_cont";
SET DEBUG_SYNC= "now WAIT_FOR con1_ready";
SET DEBUG_SYNC= "now SIGNAL con2_cont";
SET DEBUG_SYNC= "now WAIT_FOR con2_ready";
SET DEBUG_SYNC= "now SIGNAL con3_cont";
SET DEBUG_SYNC= "now WAIT_FOR con3_ready";

# Check that everything is committed in binary log.
--source include/show_binary_logs.inc
--let $binlog_file= master-bin.000003
--let $binlog_start= 4
--source include/show_binlog_events.inc
--let $binlog_file= master-bin.000004
--source include/show_binlog_events.inc
--let $binlog_file= master-bin.000005
--source include/show_binlog_events.inc
--let $binlog_file= master-bin.000006
--source include/show_binlog_events.inc


# Check that server will not purge too much.
PURGE BINARY LOGS TO "master-bin.000006";
--source include/show_binary_logs.inc

# Now crash the server with one more transaction in prepared state.
--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait-binlog_xa_recover.test
EOF
--error 0,2006,2013
SET DEBUG_SYNC= "now SIGNAL con4_cont";
connection con4;
--error 2006,2013
reap;

--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart-group_commit_binlog_pos.test
EOF

connection default;
--enable_reconnect
--source include/wait_until_connected_again.inc

# Check that all transactions are recovered.
SELECT a FROM t1 ORDER BY a;

--echo Test that with multiple binlog checkpoints, recovery starts from the last one.
SET GLOBAL max_binlog_size= 4096;
SET GLOBAL innodb_flush_log_at_trx_commit= 1;
RESET MASTER;

# Rotate to binlog master-bin.000003 while delaying binlog checkpoints.
# So we get multiple binlog checkpoints in master-bin.000003.
# Then complete the checkpoints, crash, and check that we only scan
# the necessary binlog file (ie. that we use the _last_ checkpoint).

connect(con10,localhost,root,,);
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con10_ready WAIT_FOR con10_cont";
send INSERT INTO t1 VALUES (10, REPEAT("x", 4100));

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con10_ready";

connect(con11,localhost,root,,);
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con11_ready WAIT_FOR con11_cont";
send INSERT INTO t1 VALUES (11, REPEAT("x", 4100));

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con11_ready";

connect(con12,localhost,root,,);
SET DEBUG_SYNC= "commit_after_group_release_commit_ordered SIGNAL con12_ready WAIT_FOR con12_cont";
send INSERT INTO t1 VALUES (12, REPEAT("x", 4100));

connection default;
SET DEBUG_SYNC= "now WAIT_FOR con12_ready";
INSERT INTO t1 VALUES (13, NULL);

--source include/show_binary_logs.inc
--let $binlog_file= master-bin.000004
--let $binlog_start= 4
--source include/show_binlog_events.inc

SET DEBUG_SYNC= "now SIGNAL con10_cont";
connection con10;
reap;
connection default;

# We need to sync the test case with the background processing of the
# commit checkpoint, otherwise we get nondeterministic results.
SET @old_dbug= @@global.DEBUG_DBUG;
SET GLOBAL debug_dbug="+d,binlog_background_checkpoint_processed";

SET DEBUG_SYNC= "now SIGNAL con12_cont";
connection con12;
reap;
connection default;
SET DEBUG_SYNC= "now WAIT_FOR binlog_background_checkpoint_processed";
SET GLOBAL debug_dbug= @old_dbug;

SET DEBUG_SYNC= "now SIGNAL con11_cont";
connection con11;
reap;

connection default;
# Wait for the last (master-bin.000004) binlog checkpoint to appear.
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000004"
--let $field= Info
--let $condition= = "master-bin.000004"
--source include/wait_show_condition.inc

--echo Checking that master-bin.000004 is the last binlog checkpoint
--source include/show_binlog_events.inc

--echo Now crash the server
# It is not too easy to test XA recovery, as it runs early during server
# startup, before any connections can be made.
# What we do is set a DBUG error insert which will crash if XA recovery
# starts from any other binlog than master-bin.000004 (check the file
# binlog_xa_recover-master.opt). Then we will fail here if XA recovery
# would start from the wrong place.
--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait-binlog_xa_recover.test
EOF
SET SESSION debug_dbug="+d,crash_commit_after_log";
--error 2006,2013
INSERT INTO t1 VALUES (14, NULL);

--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart-group_commit_binlog_pos.test
EOF

connection default;
--enable_reconnect
--source include/wait_until_connected_again.inc

# Check that all transactions are recovered.
SELECT a FROM t1 ORDER BY a;


--echo *** Check that recovery works if we crashed early during rotate, before
--echo *** binlog checkpoint event could be written.

SET GLOBAL max_binlog_size= 4096;
SET GLOBAL innodb_flush_log_at_trx_commit= 1;
RESET MASTER;

# We need some initial data to reach binlog master-bin.000004. Otherwise
# crash recovery fails due to the error insert used for previous test.
INSERT INTO t1 VALUES (21, REPEAT("x", 4100));
INSERT INTO t1 VALUES (22, REPEAT("x", 4100));
# Wait for the master-bin.000003 binlog checkpoint to appear.
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000003"
--let $field= Info
--let $condition= = "master-bin.000003"
--source include/wait_show_condition.inc
INSERT INTO t1 VALUES (23, REPEAT("x", 4100));
# Wait for the last (master-bin.000004) binlog checkpoint to appear.
--let $wait_for_all= 0
--let $show_statement= SHOW BINLOG EVENTS IN "master-bin.000004"
--let $field= Info
--let $condition= = "master-bin.000004"
--source include/wait_show_condition.inc

--write_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
wait-binlog_xa_recover.test
EOF
SET SESSION debug_dbug="+d,crash_before_write_checkpoint_event";
--error 2006,2013
INSERT INTO t1 VALUES (24, REPEAT("x", 4100));

--append_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
restart-group_commit_binlog_pos.test
EOF

--enable_reconnect
--source include/wait_until_connected_again.inc

# Check that all transactions are recovered.
SELECT a FROM t1 ORDER BY a;

--source include/show_binary_logs.inc
--let $binlog_file= master-bin.000004
--let $binlog_start= 4
--source include/show_binlog_events.inc

# Cleanup
connection default;
DROP TABLE t1;
