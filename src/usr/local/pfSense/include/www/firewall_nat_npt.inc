<?php
/*
 * firewall_nat_npt.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2014-2025 Rubicon Communications, LLC (Netgate)
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

// Functions to support firewall_nat_npt.php and firewall_nat_npt_edit.php

require_once("config.gui.inc");
require_once("interfaces.inc");
require_once("util.inc");
require_once("pfsense-utils.inc");
require_once("ipsec.inc");
require_once("filter.inc");

// Save NPt rule
function savenptNATrule($post, $id, $json = false) {
	if (isset($post['after'])) {
		$after = $post['after'];
	}

	unset($input_errors);
	$pconfig = $post;

	// input validation
	$reqdfields = explode(" ", "interface");
	$reqdfieldsn = array(gettext("Interface"));
	$reqdfields[] = "src";
	$reqdfieldsn[] = gettext("Source prefix");
	if (!get_specialnet($post['dsttype'])) {
		$reqdfields[] = "dst";
		$reqdfieldsn[] = gettext("Destination prefix");
	}

	if (!$json) {
		do_input_validation($post, $reqdfields, $reqdfieldsn, $input_errors);
	}

	if (!is_ipaddrv6(trim($post['src']))) {
		$input_errors[] = gettext("The specified source address is not a valid IPv6 prefix");
	}
	if (!is_ipaddrv6(trim($post['dst'])) && !get_specialnet(trim($post['dsttype']))) {
		$input_errors[] = gettext("The specified destination address is not a valid IPv6 prefix");
	}
	if (check_subnetsv6_overlap(get_interface_ipv6($post['interface']), 128, trim($post['dst']), $post['dstmask']) &&
	    !$post['dstnot']) {
		$input_errors[] = gettext("The specified destination address and interface IPv6 address cannot overlap");
	}
	$dstmask = $post['dstmask'];
	if (get_specialnet($post['dsttype'])) {
		/* Find actual destination mask from track source */
		$track6ip = get_interface_track6ip($post['dsttype']);
		$dstmask = $track6ip[1];
	}
	if (!empty($dstmask) && ($post['srcmask'] != $dstmask)) {
		$input_errors[] = gettext("The specified source prefix size must be equal to the destination prefix size.");
	}

	if (!$input_errors) {
		$natent = array();

		$natent['disabled'] = isset($post['disabled']) ? true:false;
		$natent['descr'] = $post['descr'];
		$natent['interface'] = $post['interface'];

		if ($post['src']) {
			$post['src'] = trim($post['src']);
		}
		if (get_specialnet($post['dsttype'])) {
			$post['dst'] = $post['dsttype'];
		} elseif ($post['dst']) {
			$post['dst'] = trim($post['dst']);
		}

		pconfig_to_address($natent['source'], $post['src'], $post['srcmask'], $post['srcnot']);

		pconfig_to_address($natent['destination'], $post['dst'], $post['dstmask'], $post['dstnot'], 0, 0, true);

		$a_npt = config_get_path('nat/npt', []);
		if (isset($id) && $a_npt[$id]) {
			if (isset($a_npt[$id]['labels'])) {
				// Keep rule labels.
				$natent['labels'] = $a_npt[$id]['labels'];
			}
			$a_npt[$id] = $natent;
		} else {
			if (is_numeric($after)) {
				array_splice($a_npt, $after+1, 0, array($natent));
			} else {
				$a_npt[] = $natent;
			}
		}
		config_set_path('nat/npt', $a_npt);

		if (write_config(gettext("Firewall: NAT: NPt - saved/edited NPt mapping.")) && !$json) {
			mark_subsystem_dirty('natconf');
		}
	}

	$rv = array();
	$rv['input_errors'] = $input_errors;
	$rv['pconfig'] = $pconfig;
	return $json ? json_encode($rv) : $rv;
}

// Retrieve the specified NPt rule
function getnptNATRule($id, $json = false) {
	$a_npt = config_get_path('nat/npt', []);

	$pconfig = array();

	if (isset($id) && $a_npt[$id]) {
		$pconfig['disabled'] = isset($a_npt[$id]['disabled']);

		address_to_pconfig($a_npt[$id]['source'], $pconfig['src'],
			$pconfig['srcmask'], $pconfig['srcnot'],
			$pconfig['srcbeginport'], $pconfig['srcendport']);

		address_to_pconfig($a_npt[$id]['destination'], $pconfig['dst'],
			$pconfig['dstmask'], $pconfig['dstnot'],
			$pconfig['dstbeginport'], $pconfig['dstendport']);

		$dst_arr = explode("/", $pconfig['dst']);
		if (count($dst_arr) > 1) {
			$pconfig['dst'] = $dst_arr[0];
			$pconfig['dstmask'] = $dst_arr[1];
		}

		$pconfig['interface'] = $a_npt[$id]['interface'];
		if (!$pconfig['interface']) {
			$pconfig['interface'] = "wan";
		}

		$pconfig['descr'] = $a_npt[$id]['descr'];
	} else {
		$pconfig['interface'] = "wan";
	}

	return $json ? json_encode($pconfig):$pconfig;
}

// Toggle enabled/disabled status of an NPt rule
function togglenptNATrule($post, $json = false) {
	$a_npt = config_get_path('nat/npt', []);

	if (isset($a_npt[$post['id']]['disabled'])) {
		unset($a_npt[$post['id']]['disabled']);
		$wc_msg = gettext('Firewall: NAT: NPt - enabled a NAT NPt rule.');
	} else {
		$a_npt[$post['id']]['disabled'] = true;
		$wc_msg = gettext('Firewall: NAT: NPt - disabled a NAT NPt rule.');
	}
	config_set_path('nat/npt', $a_npt);

	if (write_config($wc_msg) && !$json) {
		mark_subsystem_dirty('natconf');
	}

	if (!$json) {
		header("Location: firewall_nat_npt.php");
		exit;
	}
}

// Toggle multiple NPt rules
function toggleMultiplenptNATrules($post, $json = false) {
	$a_npt = config_get_path('nat/npt', []);

	foreach ($post['rule'] as $rulei) {
		if (isset($a_npt[$rulei]['disabled'])) {
			unset($a_npt[$rulei]['disabled']);
		} else {
			$a_npt[$rulei]['disabled'] = true;
		}
	}
	config_set_path('nat/npt', $a_npt);

	if (write_config(gettext("Firewall: NAT: NPt - toggle enable/disable for selected NPt mappings.")) && !$json) {
		mark_subsystem_dirty('natconf');
	}

	if (!$json) {
		header("Location: firewall_nat_npt.php");
		exit;
	}
}

// Delete multiple NPt rules
function deleteMultiplenptNATrules($post, $json = false) {
	$a_npt = config_get_path('nat/npt', []);

	foreach ($post['rule'] as $rulei) {
		unset($a_npt[$rulei]);
	}
	config_set_path('nat/npt', $a_npt);

	if (write_config(gettext("Firewall: NAT: NPt - deleted selected NPt mappings.")) && !$json) {
		mark_subsystem_dirty('natconf');
	}

	if (!$json) {
		header("Location: firewall_nat_npt.php");
		exit;
	}
}

// Delete NPt rule
function deletenptNATrule($post, $json = false) {
	config_del_path("nat/npt/{$post['id']}");
	if (write_config(gettext("Firewall: NAT: NPt - deleted NPt mapping.")) && !$json) {
		mark_subsystem_dirty('natconf');
	}

	if(!$json) {
		header("Location: firewall_nat_npt.php");
		exit;
	}
}

// Re-order the NPtNAT rules per the array of indices passed in $post
function reordernptNATrules($post, $json = false) {
	if (is_array($post['rule']) && !empty($post['rule'])) {
		$a_npt = config_get_path('nat/npt', []);
		$a_npt_new = array();

		// if a rule is not in POST[rule], it has been deleted by the user
		foreach ($post['rule'] as $id) {
			$a_npt_new[] = $a_npt[$id];
		}

		$a_npt = $a_npt_new;
		config_set_path('nat/npt', $a_npt);

		if (write_config(gettext("Firewall: NAT: NPt - reordered NPt mappings.")) && !$json) {
			mark_subsystem_dirty('natconf');
		}

		if (!$json) {
			header("Location: firewall_nat_npt.php");
			exit;
		}
	}
}

function applynptNATrules() {
	$retval = 0;
	$retval |= filter_configure();

	if ($retval == 0) {
		clear_subsystem_dirty('natconf');
		clear_subsystem_dirty('filter');
	}

	return $retval;
}

?>
