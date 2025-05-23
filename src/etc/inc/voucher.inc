<?php
/*
 * voucher.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2007-2013 BSD Perimeter
 * Copyright (c) 2013-2016 Electric Sheep Fencing
 * Copyright (c) 2014-2025 Rubicon Communications, LLC (Netgate)
 * Copyright (c) 2007 Marcel Wiget <mwiget@mac.com>
 * All rights reserved.
 *
 * originally part of m0n0wall (http://m0n0.ch/wall)
 * Copyright (c) 2003-2004 Manuel Kasper <mk@neon1.net>.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* include all configuration functions */
if (!function_exists('captiveportal_syslog')) {
	require_once("captiveportal.inc");
}

function captiveportal_xmlrpc_sync_get_details(&$syncip, &$port, &$username, &$password, $carp_loop = false) {
	global $cpzone;

	if (config_path_enabled("captiveportal/{$cpzone}", "enablebackwardsync") && !$carp_loop
	    && captiveportal_ha_is_node_in_backup_mode($cpzone)) {
		$syncip = config_get_path("captiveportal/{$cpzone}/backwardsyncip");
		$username = config_get_path("captiveportal/{$cpzone}/backwardsyncuser");
		$password = config_get_path("captiveportal/{$cpzone}/backwardsyncpassword");
		$port = config_get_path('system/webgui/port');
		if (empty($port)) { // if port is empty lets rely on the protocol selection
			if (config_get_path('system/webgui/protocol') == "http") {
				$port = "80";
			} else {
				$port = "443";
			}
		}
		return true;
	}

	if (empty(config_get_path('hasync/synchronizetoip')) ||
	    config_get_path('hasync/synchronizecaptiveportal') == "" ||
	    $carp_loop == true) {
		return false;
	}

	$syncip = config_get_path('hasync/synchronizetoip');
	$password = config_get_path('hasync/password');
	if (empty(config_get_path('hasync/username'))) {
		$username = "admin";
	} else {
		$username = config_get_path('hasync/username');
	}

	/* if port is empty lets rely on the protocol selection */
	$port = config_get_path('system/webgui/port');
	if (empty($port)) {
		if (config_get_path('system/webgui/protocol') == "http") {
			$port = "80";
		} else {
			$port = "443";
		}
	}

	return true;
}

function voucher_expire($voucher_received, $carp_loop = false) {
	global $g, $cpzone, $cpzoneid;

	$voucherlck = lock("voucher{$cpzone}", LOCK_EX);

	// read rolls into assoc array with rollid as key and minutes as value
	$tickets_per_roll = array();
	$minutes_per_roll = array();
	foreach (config_get_path("voucher/{$cpzone}/roll", []) as $rollent) {
		$tickets_per_roll[$rollent['number']] = $rollent['count'];
		$minutes_per_roll[$rollent['number']] = $rollent['minutes'];
	}

	// split into an array. Useful for multiple vouchers given
	$a_vouchers_received = preg_split("/[\t\n\r ]+/s", $voucher_received);
	$active_dirty = false;
	$unsetindexes = array();

	// go through all received vouchers, check their valid and extract
	// Roll# and Ticket# using the external readvoucher binary
	foreach ($a_vouchers_received as $voucher) {
		$v = escapeshellarg($voucher);
		if (strlen($voucher) < 5) {
			captiveportal_syslog("{$voucher} invalid: Too short!");
			continue;   // seems too short to be a voucher!
		}

		unset($output);
		$_gb = exec("/usr/local/bin/voucher -c {$g['varetc_path']}/voucher_{$cpzone}.cfg -k {$g['varetc_path']}/voucher_{$cpzone}.public -- $v", $output);
		list($status, $roll, $nr) = explode(" ", $output[0]);
		if ($status == "OK") {
			// check if we have this ticket on a registered roll for this ticket
			if ($tickets_per_roll[$roll] && ($nr <= $tickets_per_roll[$roll])) {
				// voucher is from a registered roll.
				if (!isset($active_vouchers[$roll])) {
					$active_vouchers[$roll] = voucher_read_active_db($roll);
				}
				// valid voucher. Store roll# and ticket#
				if (!empty($active_vouchers[$roll][$voucher])) {
					$active_dirty = true;
					unset($active_vouchers[$roll][$voucher]);
				}
				// check if voucher already marked as used
				if (!isset($bitstring[$roll])) {
					$bitstring[$roll] = voucher_read_used_db($roll);
				}
				$pos = $nr >> 3; // divide by 8 -> octet
				$mask = 1 << ($nr % 8);
				// mark bit for this voucher as used
				if (!(ord($bitstring[$roll][$pos]) & $mask)) {
					$bitstring[$roll][$pos] = chr(ord($bitstring[$roll][$pos]) | $mask);
				}
				captiveportal_syslog("{$voucher} ({$roll}/{$nr}) forced to expire");

				/* Check if this voucher has any active sessions */
				$cpentry = captiveportal_read_db("WHERE username = '{$voucher}'");
				if (!empty($cpentry) && !empty($cpentry[0])) {
					if (empty($cpzoneid) && !empty(config_get_path("captiveportal/{$cpzone}"))) {
						$cpzoneid = config_get_path("captiveportal/{$cpzone}/zoneid");
					}
					$cpentry = $cpentry[0];
					captiveportal_disconnect($cpentry, 13);
					captiveportal_logportalauth($cpentry[4], $cpentry[3], $cpentry[2], "FORCEFULLY TERMINATING VOUCHER {$voucher} SESSION");
					$unsetindexes[] = $cpentry[5];
				}
			} else {
				captiveportal_syslog(sprintf(gettext('%1$s (%2$s/%3$s): not found on any registered Roll'), $voucher, $roll, $nr));
			}
		} else {
			// hmm, thats weird ... not what I expected
			captiveportal_syslog(sprintf(gettext('%1$s invalid: %2$s!!'), $voucher, $output[0]));
		}
	}

	// Refresh active DBs
	if ($active_dirty == true) {
		foreach ($active_vouchers as $roll => $active) {
			voucher_write_active_db($roll, $active);
		}
		/* perform in-use vouchers expiration using check_reload_status */
		send_event("service sync vouchers");
	} else {
		$active_vouchers = array();
	}

	// Write back the used DB's
	if (is_array($bitstring)) {
		foreach ($bitstring as $roll => $used) {
			if (is_array($used)) {
				foreach ($used as $u) {
					voucher_write_used_db($roll, base64_encode($u));
				}
			} else {
				voucher_write_used_db($roll, base64_encode($used));
			}
		}
	}

	unlock($voucherlck);

	/* Write database */
	if (!empty($unsetindexes)) {
		captiveportal_remove_entries($unsetindexes);
	}

	// XMLRPC Call over to the other node
	if (captiveportal_xmlrpc_sync_get_details($syncip, $syncport,
	    $syncuser, $syncpass, $carp_loop)) {
		$rpc_client = new pfsense_xmlrpc_client();
		$rpc_client->setConnectionData($syncip, $syncport, $syncuser, $syncpass);
		$rpc_client->set_noticefile("CaptivePortalVouchersSync");
		$arguments = array(
			'active_and_used_vouchers_bitmasks' => $bitstring,
			'active_vouchers' => $active_vouchers
		);

		$rpc_client->xmlrpc_method('captive_portal_sync',
			array(
				'op' => 'write_vouchers',
				'zone' => $cpzone,
				'arguments' => base64_encode(serialize($arguments))
			)
		);
	}
	return true;
}

/*
 * Authenticate a voucher and return the remaining time credit in minutes
 * if $test is set, don't mark the voucher as used nor add it to the list
 * of active vouchers
 * If $test is set, simply test the voucher. Don't change anything
 * but return a more verbose error and result message back
 */
function voucher_auth($voucher_received, $test = 0, $carp_loop = false) {
	global $g, $cpzone, $dbc;

	if (!config_path_enabled("voucher/{$cpzone}")) {
		return 0;
	}

	$voucherlck = lock("voucher{$cpzone}", LOCK_EX);

	// read rolls into assoc array with rollid as key and minutes as value
	$tickets_per_roll = array();
	$minutes_per_roll = array();
	foreach (config_get_path("voucher/{$cpzone}/roll", []) as $rollent) {
		$tickets_per_roll[$rollent['number']] = $rollent['count'];
		$minutes_per_roll[$rollent['number']] = $rollent['minutes'];
	}

	// split into an array. Useful for multiple vouchers given
	$a_vouchers_received = preg_split("/[\t\n\r ]+/s", $voucher_received);
	$error = 0;
	$test_result = array();     // used to display for voucher test option in GUI
	$total_minutes = 0;
	$first_voucher = "";
	$first_voucher_roll = 0;

	// go through all received vouchers, check their valid and extract
	// Roll# and Ticket# using the external readvoucher binary
	foreach ($a_vouchers_received as $voucher) {
		$v = escapeshellarg($voucher);
		if (strlen($voucher) < 5) {
			$voucher_err_text = sprintf(gettext("%s invalid: Too short!"), $voucher);
			$test_result[] = $voucher_err_text;
			captiveportal_syslog($voucher_err_text);
			$error++;
			continue;   // seems too short to be a voucher!
		}

		$result = exec("/usr/local/bin/voucher -c {$g['varetc_path']}/voucher_{$cpzone}.cfg -k {$g['varetc_path']}/voucher_{$cpzone}.public -- $v");
		list($status, $roll, $nr) = explode(" ", $result);
		if ($status == "OK") {
			if (!$first_voucher) {
				// store first voucher. Thats the one we give the timecredit
				$first_voucher = $voucher;
				$first_voucher_roll = $roll;
			}
			// check if we have this ticket on a registered roll for this ticket
			if ($tickets_per_roll[$roll] && ($nr <= $tickets_per_roll[$roll])) {
				// voucher is from a registered roll.
				if (!isset($active_vouchers[$roll])) {
					$active_vouchers[$roll] = voucher_read_active_db($roll);
				}
				// valid voucher. Store roll# and ticket#
				if (!empty($active_vouchers[$roll][$voucher])) {
					list($timestamp, $minutes) = explode(",", $active_vouchers[$roll][$voucher]);
					// we have an already active voucher here.
					$remaining = intval((($timestamp + (60*$minutes)) - time())/60);
					$test_result[] = sprintf(gettext('%1$s (%2$s/%3$s) active and good for %4$d Minutes'), $voucher, $roll, $nr, $remaining);
					$total_minutes += $remaining;
				} else {
					// voucher not used. Check if ticket Id is on the roll (not too high)
					// and if the ticket is marked used.
					// check if voucher already marked as used
					if (!isset($bitstring[$roll])) {
						$bitstring[$roll] = voucher_read_used_db($roll);
					}
					$pos = $nr >> 3; // divide by 8 -> octet
					$mask = 1 << ($nr % 8);
					if (ord($bitstring[$roll][$pos]) & $mask) {
						$voucher_err_text = sprintf(gettext('%1$s (%2$s/%3$s) already used and expired'), $voucher, $roll, $nr);
						$test_result[] = $voucher_err_text;
						captiveportal_syslog($voucher_err_text);
						$total_minutes = -1;    // voucher expired
						$error++;
					} else {
						// mark bit for this voucher as used
						$bitstring[$roll][$pos] = chr(ord($bitstring[$roll][$pos]) | $mask);
						$test_result[] = sprintf(gettext('%1$s (%2$s/%3$s) good for %4$s Minutes'), $voucher, $roll, $nr, $minutes_per_roll[$roll]);
						$total_minutes += $minutes_per_roll[$roll];
					}
				}
			} else {
				$voucher_err_text = sprintf(gettext('%1$s (%2$s/%3$s): not found on any registered Roll'), $voucher, $roll, $nr);
				$test_result[] = $voucher_err_text;
				captiveportal_syslog($voucher_err_text);
			}
		} else {
			// hmm, thats weird ... not what I expected
			$voucher_err_text = sprintf(gettext('%1$s invalid: %2$s !!'), $voucher, $result);
			$test_result[] = $voucher_err_text;
			captiveportal_syslog($voucher_err_text);
			$error++;
		}
	}

	// if this was a test call, we're done. Return the result.
	if ($test) {
		if ($error) {
			$test_result[] = gettext("Access denied!");
		} else {
			$test_result[] = sprintf(gettext("Access granted for %d Minutes in total."), $total_minutes);
		}
		unlock($voucherlck);

		return $test_result;
	}

	// if we had an error (one of the vouchers is invalid), return 0.
	// Discussion: we could return the time remaining for good vouchers, but then
	// the user wouldn't know that he used at least one invalid voucher.
	if ($error) {
		unlock($voucherlck);
		if ($total_minutes > 0) {   // probably not needed, but want to make sure
			$total_minutes = 0;     // we only report -1 (expired) or 0 (no access)
		}
		return $total_minutes;       // well, at least one voucher had errors. Say NO ACCESS
	}

	// All given vouchers were valid and this isn't simply a test.
	// Write back the used DB's
	if (is_array($bitstring)) {
		foreach ($bitstring as $roll => $used) {
			if (is_array($used)) {
				foreach ($used as $u) {
					voucher_write_used_db($roll, base64_encode($u));
				}
			} else {
				voucher_write_used_db($roll, base64_encode($used));
			}
		}
	}

	// Active DB: we only add the first voucher if multiple given
	// and give that one all the time credit. This allows the user to logout and
	// log in later using just the first voucher. It also keeps username limited
	// to one voucher and that voucher shows the correct time credit in 'active vouchers'
	if (!empty($active_vouchers[$first_voucher_roll][$first_voucher])) {
		list($timestamp, $minutes) = explode(",", $active_vouchers[$first_voucher_roll][$first_voucher]);
	} else {
		$timestamp = time();    // new voucher
		$minutes = $total_minutes;
	}

	$active_vouchers[$first_voucher_roll][$first_voucher] = "$timestamp,$minutes";
	voucher_write_active_db($first_voucher_roll, $active_vouchers[$first_voucher_roll]);

	// XMLRPC Call over to the other node
	if (captiveportal_xmlrpc_sync_get_details($syncip, $syncport,
	    $syncuser, $syncpass, $carp_loop)) {
		$rpc_client = new pfsense_xmlrpc_client();
		$rpc_client->setConnectionData($syncip, $syncport, $syncuser, $syncpass);
		$rpc_client->set_noticefile("CaptivePortalVouchersSync");
		$arguments = array(
			'active_and_used_vouchers_bitmasks' => $bitstring,
			'active_vouchers' => array(
				$first_voucher_roll => $active_vouchers[$first_voucher_roll]
			)
		);

		$rpc_client->xmlrpc_method('captive_portal_sync',
			array(
				'op' => 'write_vouchers',
				'zone' => $cpzone,
				'arguments' => base64_encode(serialize($arguments))
			)
		);
	}

	/* perform in-use vouchers expiration using check_reload_status */
	send_event("service sync vouchers");

	unlock($voucherlck);

	return $total_minutes;
}

function voucher_configure() {
	global $g, $cpzone;

	foreach (config_get_path('voucher', []) as $voucherzone => $vcfg) {
		if (is_platform_booting()) {
			echo gettext("Enabling voucher support... ");
		}
		$cpzone = $voucherzone;
		$error = voucher_configure_zone();
		if (is_platform_booting()) {
			if ($error) {
				echo "error\n";
			} else {
				echo "done\n";
			}
		}
	}
}

function voucher_configure_zone() {
	global $g, $cpzone;

	if (!config_path_enabled("voucher/{$cpzone}")) {
		return 0;
	}

	$voucherlck = lock("voucher{$cpzone}", LOCK_EX);

	/* write public key used to verify vouchers */
	$pubkey = base64_decode(config_get_path("voucher/{$cpzone}/publickey"));
	$fd = fopen("{$g['varetc_path']}/voucher_{$cpzone}.public", "w");
	if (!$fd) {
		captiveportal_syslog("Voucher error: cannot write voucher.public");
		unlock($voucherlck);
		return 1;
	}
	fwrite($fd, $pubkey);
	fclose($fd);
	@chmod("{$g['varetc_path']}/voucher_{$cpzone}.public", 0600);

	/* write config file used by voucher binary to decode vouchers */
	$fd = fopen("{$g['varetc_path']}/voucher_{$cpzone}.cfg", "w");
	if (!$fd) {
		printf(gettext("Error: cannot write voucher.cfg") . "\n");
		unlock($voucherlck);
		return 1;
	}
	$voucher_config = config_get_path("voucher/{$cpzone}", []);
	fwrite($fd, "{$voucher_config['rollbits']},{$voucher_config['ticketbits']},{$voucher_config['checksumbits']},{$voucher_config['magic']},{$voucher_config['charset']}\n");
	fclose($fd);
	@chmod("{$g['varetc_path']}/voucher_{$cpzone}.cfg", 0600);
	unlock($voucherlck);

	return 0;
}

/* write bitstring of used vouchers to ramdisk.
 * Bitstring must already be base64_encoded!
 */
function voucher_write_used_db($roll, $vdb) {
	global $g, $cpzone;

	$fd = fopen("{$g['vardb_path']}/voucher_{$cpzone}_used_$roll.db", "w");
	if ($fd) {
		fwrite($fd, $vdb . "\n");
		fclose($fd);
	} else {
		voucher_log(LOG_ERR, sprintf(gettext('cant write %1$s/voucher_%2$s_used_%3$s.db'), g_get('vardb_path'), $cpzone, $roll));
	}
}

/* return assoc array of active vouchers with activation timestamp
 * voucher is index.
 */
function voucher_read_active_db($roll) {
	global $g, $cpzone;

	$active = array();
	$dirty = 0;
	$file = "{$g['vardb_path']}/voucher_{$cpzone}_active_$roll.db";
	if (file_exists($file)) {
		$fd = fopen($file, "r");
		if ($fd) {
			while (!feof($fd)) {
				$line = trim(fgets($fd));
				if ($line) {
					list($voucher, $timestamp, $minutes) = explode(",", $line); // voucher,timestamp
					if ((($timestamp + (60*$minutes)) - time()) > 0) {
						$active[$voucher] = "$timestamp,$minutes";
					} else {
						$dirty=1;
					}
				}
			}
			fclose($fd);
			if ($dirty) { // if we found expired entries, lets remove them
				voucher_write_active_db($roll, $active);
			}
		}
	}
	return $active;
}

/* store array of active vouchers back to DB */
function voucher_write_active_db($roll, $active) {
	global $g, $cpzone;

	if (!is_array($active)) {
		return;
	}
	$fd = fopen("{$g['vardb_path']}/voucher_{$cpzone}_active_$roll.db", "w");
	if ($fd) {
		foreach ($active as $voucher => $value) {
			fwrite($fd, "$voucher,$value\n");
		}
		fclose($fd);
	}
}

/* return how many vouchers are marked used on a roll */
function voucher_used_count($roll) {
	global $g, $cpzone;

	$bitstring = voucher_read_used_db($roll);
	$max = strlen($bitstring) * 8;
	$used = 0;
	for ($i = 1; $i <= $max; $i++) {
		// check if ticket already used or not.
		$pos = $i >> 3;            // divide by 8 -> octet
		$mask = 1 << ($i % 8);  // mask to test bit in octet
		if (ord($bitstring[$pos]) & $mask) {
			$used++;
		}
	}
	unset($bitstring);

	return $used;
}

function voucher_read_used_db($roll) {
	global $g, $cpzone;

	$vdb = "";
	$file = "{$g['vardb_path']}/voucher_{$cpzone}_used_$roll.db";
	if (file_exists($file)) {
		$fd = fopen($file, "r");
		if ($fd) {
			$vdb = trim(fgets($fd));
			fclose($fd);
		} else {
			voucher_log(LOG_ERR, sprintf(gettext('cant read %1$s/voucher_%2$s_used_%3$s.db'), g_get('vardb_path'), $cpzone, $roll));
		}
	}
	return base64_decode($vdb);
}

function voucher_unlink_db($roll) {
	global $g, $cpzone;
	@unlink("{$g['vardb_path']}/voucher_{$cpzone}_used_$roll.db");
	@unlink("{$g['vardb_path']}/voucher_{$cpzone}_active_$roll.db");
}

/* we share the log with captiveportal for now */
function voucher_log($priority, $message) {

	$message = trim($message);
	openlog("logportalauth", LOG_PID, LOG_LOCAL4);
	syslog($priority, sprintf(gettext("Voucher: %s"), $message));
	closelog();
}

/* Perform natural expiration of vouchers
 * Called during reboot -> system_reboot_cleanup() and every active voucher change
 */
function voucher_save_db_to_config() {
	global $g;

	foreach (config_get_path('voucher', []) as $zone => $vcfg) {
		if (config_path_enabled("voucher/{$zone}")) {
			foreach (config_get_path("voucher/{$zone}/roll", []) as $key => $rollent) {
				// read_active_db will remove expired vouchers that are still active
				voucher_read_active_db($rollent['number']);
			}
		}
	}
}

?>
