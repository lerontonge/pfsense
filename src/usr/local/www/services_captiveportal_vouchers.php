<?php
/*
 * services_captiveportal_vouchers.php
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2004-2013 BSD Perimeter
 * Copyright (c) 2013-2016 Electric Sheep Fencing
 * Copyright (c) 2014-2025 Rubicon Communications, LLC (Netgate)
 * Copyright (c) 2007 Marcel Wiget <mwiget@mac.com>
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

##|+PRIV
##|*IDENT=page-services-captiveportal-vouchers
##|*NAME=Services: Captive Portal Vouchers
##|*DESCR=Allow access to the 'Services: Captive Portal Vouchers' page.
##|*MATCH=services_captiveportal_vouchers.php*
##|-PRIV

if ($_POST['postafterlogin']) {
	$nocsrf= true;
}

require_once("guiconfig.inc");
require_once("functions.inc");
require_once("filter.inc");
require_once("shaper.inc");
require_once("captiveportal.inc");
require_once("voucher.inc");

$cpzone = strtolower(htmlspecialchars($_REQUEST['zone']));

if ($_POST['generatekey']) {
	include_once("phpseclib/Math/BigInteger.php");
	include_once("phpseclib/Crypt/Hash.php");
	include_once("phpseclib/Crypt/RSA.php");

	$rsa = new phpseclib\Crypt\RSA();
	$key = $rsa->createKey(64);
	$privatekey = $key["privatekey"];
	$publickey = $key["publickey"];

	print json_encode(['public' => $publickey, 'private' => $privatekey]);
	exit;
}

if (empty($cpzone)) {
	header("Location: services_captiveportal_zones.php");
	exit;
}

if (empty(config_get_path("captiveportal/{$cpzone}"))) {
	log_error(sprintf(gettext("Submission on captiveportal page with unknown zone parameter: %s"), htmlspecialchars($cpzone)));
	header("Location: services_captiveportal_zones.php");
	exit;
}

$pgtitle = array(gettext("Services"), gettext("Captive Portal"), htmlspecialchars($cpzone), gettext("Vouchers"));
$pglinks = array("", "services_captiveportal_zones.php", "services_captiveportal.php?zone=" . $cpzone, "@self");
$shortcut_section = "captiveportal-vouchers";

$voucher_config = config_get_path("voucher/{$cpzone}", []);
if (!isset($voucher_config['charset'])) {
	$voucher_config['charset'] = '2345678abcdefhijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ';
}
if (!isset($voucher_config['rollbits'])) {
	$voucher_config['rollbits'] = 16;
}
if (!isset($voucher_config['ticketbits'])) {
	$voucher_config['ticketbits'] = 10;
}
if (!isset($voucher_config['checksumbits'])) {
	$voucher_config['checksumbits'] = 5;
}
if (!isset($voucher_config['magic'])) {
	$voucher_config['magic'] = rand();	 // anything slightly random will do
}
if (!isset($voucher_config['exponent'])) {
	while (true) {
		while (($exponent = rand()) % 30000 < 5000) {
			continue;
		}
		$exponent = ($exponent * 2) + 1; // Make it odd number
		if ($exponent <= 65537) {
			break;
		}
	}

	$voucher_config['exponent'] = $exponent;
	unset($exponent);
}

if (!isset($voucher_config['publickey'])) {
	/* generate a random 64 bit RSA key pair using the voucher binary */
	$fd = popen("/usr/local/bin/voucher -g 64 -e " . $voucher_config['exponent'], "r");
	if ($fd !== false) {
		$output = fread($fd, 16384);
		pclose($fd);
		list($privkey, $pubkey) = explode("\0", $output);
		$voucher_config['publickey'] = base64_encode($pubkey);
		$voucher_config['privatekey'] = base64_encode($privkey);
	}
}

// Check for invalid or expired vouchers
if (!isset($voucher_config['descrmsgnoaccess'])) {
	$voucher_config['descrmsgnoaccess'] = gettext("Voucher invalid");
}

if (!isset($voucher_config['descrmsgexpired'])) {
	$voucher_config['descrmsgexpired'] = gettext("Voucher expired");
}

config_set_path("voucher/{$cpzone}", $voucher_config);

if (($_POST['act'] == "del") && is_numericint($_POST['id'])) {
	$id = $_POST['id'];
	if (config_get_path("voucher/{$cpzone}/roll/{$id}")) {
		$roll = config_get_path("voucher/{$cpzone}/roll/{$id}/number");
		$voucherlck = lock("voucher{$cpzone}");
		config_del_path("voucher/{$cpzone}/roll/{$id}");
		voucher_unlink_db($roll);
		unlock($voucherlck);
		write_config("Deleted voucher roll");
	}
	header("Location: services_captiveportal_vouchers.php?zone={$cpzone}");
	exit;
} else if ($_REQUEST['act'] == "csv") {
	/* print all vouchers of the selected roll */
	$privkey = base64_decode($voucher_config['privatekey']);
	if (strstr($privkey, "BEGIN RSA PRIVATE KEY")) {
		$fd = fopen("{$g['varetc_path']}/voucher_{$cpzone}.private", "w");
		if (!$fd) {
			$input_errors[] = gettext("Cannot write private key file") . ".\n";
		} else {
			chmod("{$g['varetc_path']}/voucher_{$cpzone}.private", 0600);
			fwrite($fd, $privkey);
			fclose($fd);
			$id = is_numericint($_REQUEST['id']) ? $_REQUEST['id'] : null;
			$this_voucher = isset($id) ? config_get_path("voucher/{$cpzone}/roll/{$id}") : null;
			if ($this_voucher) {
				$number = $this_voucher['number'];
				$count = $this_voucher['count'];
				if (file_exists("{$g['varetc_path']}/voucher_{$cpzone}.cfg")) {
					$cmd = "/usr/local/bin/voucher" .
						" -c " . escapeshellarg("{$g['varetc_path']}/voucher_{$cpzone}.cfg") .
						" -p " . escapeshellarg("{$g['varetc_path']}/voucher_{$cpzone}.private") .
						" " . escapeshellarg($number) .
						" " . escapeshellarg($count);
					send_user_download('data',
						preg_replace('/" /', '"', shell_exec($cmd)),
						"vouchers_{$cpzone}_roll{$number}.csv");
				}
				@unlink("{$g['varetc_path']}/voucher_{$cpzone}.private");
			} else {
				header("Location: services_captiveportal_vouchers.php?zone={$cpzone}");
			}
			exit;
		}
	} else {
		$input_errors[] = gettext("Need private RSA key to print vouchers") . "\n";
	}
}

$pconfig['enable'] = config_path_enabled("voucher/{$cpzone}");
$pconfig['charset'] = config_get_path("voucher/{$cpzone}/charset");
$pconfig['rollbits'] = config_get_path("voucher/{$cpzone}/rollbits");
$pconfig['ticketbits'] = config_get_path("voucher/{$cpzone}/ticketbits");
$pconfig['checksumbits'] = config_get_path("voucher/{$cpzone}/checksumbits");
$pconfig['magic'] = config_get_path("voucher/{$cpzone}/magic");
$pconfig['exponent'] = config_get_path("voucher/{$cpzone}/exponent");
$pconfig['publickey'] = base64_decode(config_get_path("voucher/{$cpzone}/publickey"));
$pconfig['privatekey'] = base64_decode(config_get_path("voucher/{$cpzone}/privatekey"));
$pconfig['msgnoaccess'] = config_get_path("voucher/{$cpzone}/descrmsgnoaccess");
$pconfig['msgexpired'] = config_get_path("voucher/{$cpzone}/descrmsgexpired");

if ($_POST['save']) {
	unset($input_errors);

	if ($_POST['postafterlogin']) {
		voucher_expire($_POST['voucher_expire']);
		exit;
	}

	$pconfig = $_POST;

	/* input validation */
	if ($_POST['enable'] == "yes") {
		$reqdfields = explode(" ", "charset rollbits ticketbits checksumbits publickey magic");
		$reqdfieldsn = array(gettext("charset"), gettext("rollbits"), gettext("ticketbits"), gettext("checksumbits"), gettext("publickey"), gettext("magic"));

		do_input_validation($_POST, $reqdfields, $reqdfieldsn, $input_errors);
	}

	// Check for form errors
	if ($_POST['charset'] && (strlen($_POST['charset']) < 2)) {
		$input_errors[] = gettext("Need at least 2 characters to create vouchers.");
	}
	if ($_POST['charset'] && (strpos($_POST['charset'], "\"") > 0)) {
		$input_errors[] = gettext("Double quotes aren't allowed.");
	}
	if ($_POST['charset'] && (strpos($_POST['charset'], ",") > 0)) {
		$input_errors[] = gettext("',' aren't allowed.");
	}
	if ($_POST['rollbits'] && (!is_numeric($_POST['rollbits']) || ($_POST['rollbits'] < 1) || ($_POST['rollbits'] > 31))) {
		$input_errors[] = gettext("# of Bits to store Roll Id needs to be between 1..31.");
	}
	if ($_POST['ticketbits'] && (!is_numeric($_POST['ticketbits']) || ($_POST['ticketbits'] < 1) || ($_POST['ticketbits'] > 16))) {
		$input_errors[] = gettext("# of Bits to store Ticket Id needs to be between 1..16.");
	}
	if ($_POST['checksumbits'] && (!is_numeric($_POST['checksumbits']) || ($_POST['checksumbits'] < 1) || ($_POST['checksumbits'] > 31))) {
		$input_errors[] = gettext("# of Bits to store checksum needs to be between 1..31.");
	}
	if ($_POST['publickey'] && (!strstr($_POST['publickey'], "BEGIN PUBLIC KEY"))) {
		$input_errors[] = gettext("This doesn't look like an RSA Public key.");
	}
	if ($_POST['privatekey'] && (!strstr($_POST['privatekey'], "BEGIN RSA PRIVATE KEY"))) {
		$input_errors[] = gettext("This doesn't look like an RSA Private key.");
	}

	if (!$input_errors) {
		$newvoucher = config_get_path("voucher/{$cpzone}", []);
		if ($_POST['enable'] == "yes") {
			$newvoucher['enable'] = true;
		} else {
			unset($newvoucher['enable']);
		}
		$newvoucher['charset'] = $_POST['charset'];
		$newvoucher['rollbits'] = $_POST['rollbits'];
		$newvoucher['ticketbits'] = $_POST['ticketbits'];
		$newvoucher['checksumbits'] = $_POST['checksumbits'];
		$newvoucher['magic'] = $_POST['magic'];
		$newvoucher['exponent'] = $_POST['exponent'];
		$newvoucher['publickey'] = base64_encode($_POST['publickey']);
		$newvoucher['privatekey'] = base64_encode($_POST['privatekey']);
		$newvoucher['descrmsgnoaccess'] = $_POST['msgnoaccess'];
		$newvoucher['descrmsgexpired'] = $_POST['msgexpired'];
		config_set_path("voucher/{$cpzone}", $newvoucher);
		write_config('Updated vouchers settings');
		voucher_configure_zone();
		// Refresh captiveportal login to show voucher changes
		captiveportal_configure_zone(config_get_path("captiveportal/{$cpzone}", []));

		if (!$input_errors) {
			header("Location: services_captiveportal_vouchers.php?zone={$cpzone}");
			exit;
		}
	}
}

include("head.inc");

if ($input_errors) {
	print_input_errors($input_errors);
}

if ($savemsg) {
	print_info_box($savemsg, 'success');
}

$tab_array = array();
$tab_array[] = array(gettext("Configuration"), false, "services_captiveportal.php?zone={$cpzone}");
$tab_array[] = array(gettext("MACs"), false, "services_captiveportal_mac.php?zone={$cpzone}");
$tab_array[] = array(gettext("Allowed IP Addresses"), false, "services_captiveportal_ip.php?zone={$cpzone}");
$tab_array[] = array(gettext("Allowed Hostnames"), false, "services_captiveportal_hostname.php?zone={$cpzone}");
$tab_array[] = array(gettext("Vouchers"), true, "services_captiveportal_vouchers.php?zone={$cpzone}");
$tab_array[] = array(gettext("High Availability"), false, "services_captiveportal_hasync.php?zone={$cpzone}");
$tab_array[] = array(gettext("File Manager"), false, "services_captiveportal_filemanager.php?zone={$cpzone}");
display_top_tabs($tab_array, true);

// We draw a simple table first, then present the controls to work with it
?>
<div class="panel panel-default">
	<div class="panel-heading"><h2 class="panel-title"><?=gettext("Voucher Rolls");?></h2></div>
	<div class="panel-body">
		<div class="table-responsive">
			<table class="table table-striped table-hover table-condensed table-rowdblclickedit sortable-theme-bootstrap" data-sortable>
				<thead>
					<tr>
						<th><?=gettext("Roll #")?></th>
						<th><?=gettext("Minutes/Ticket")?></th>
						<th><?=gettext("# of Tickets")?></th>
						<th><?=gettext("Comment")?></th>
						<th><?=gettext("Actions")?></th>
					</tr>
				</thead>
				<tbody>
<?php
$i = 0;
foreach (config_get_path("voucher/{$cpzone}/roll", []) as $rollent):
?>
					<tr>
						<td><?=htmlspecialchars($rollent['number']); ?></td>
						<td><?=htmlspecialchars($rollent['minutes'])?></td>
						<td><?=htmlspecialchars($rollent['count'])?></td>
						<td><?=htmlspecialchars($rollent['descr']); ?></td>
						<td>
							<!-- These buttons are hidden/shown on checking the 'enable' checkbox -->
							<a class="fa-solid fa-pencil"		title="<?=gettext("Edit voucher roll"); ?>" href="services_captiveportal_vouchers_edit.php?zone=<?=$cpzone?>&amp;id=<?=$i; ?>"></a>
							<a class="fa-solid fa-trash-can"		title="<?=gettext("Delete voucher roll")?>" href="services_captiveportal_vouchers.php?zone=<?=$cpzone?>&amp;act=del&amp;id=<?=$i; ?>" usepost></a>
							<a class="fa-regular fa-file-excel"	title="<?=gettext("Export vouchers for this roll to a .csv file")?>" href="services_captiveportal_vouchers.php?zone=<?=$cpzone?>&amp;act=csv&amp;id=<?=$i; ?>"></a>
						</td>
					</tr>
<?php
	$i++;
endforeach;
?>
				</tbody>
			</table>
		</div>
	</div>
</div>
<?php

if ($pconfig['enable']) : ?>
	<nav class="action-buttons">
		<a href="services_captiveportal_vouchers_edit.php?zone=<?=$cpzone?>" class="btn btn-success">
			<i class="fa-solid fa-plus icon-embed-btn"></i>
			<?=gettext("Add")?>
		</a>
	</nav>
<?php
endif;

$form = new Form();

$section = new Form_Section('Create, Generate and Activate Rolls with Vouchers');

$section->addInput(new Form_Checkbox(
	'enable',
	'Enable',
	'Enable the creation, generation and activation of rolls with vouchers',
	$pconfig['enable']
	));

$form->add($section);

$section = new Form_Section('Create, Generate and Activate Rolls with Vouchers');
$section->addClass('rolledit');

$section->addInput(new Form_Textarea(
	'publickey',
	'Voucher Public Key',
	$pconfig['publickey']
))->setHelp('Paste an RSA public key (64 Bit or smaller) in PEM format here. This key is used to decrypt vouchers.');

$section->addInput(new Form_Textarea(
	'privatekey',
	'Voucher Private Key',
	$pconfig['privatekey']
))->setHelp('Paste an RSA private key (64 Bit or smaller) in PEM format here. This key is only used to generate encrypted vouchers and doesn\'t need to be available if the vouchers have been generated offline.');

$section->addInput(new Form_Input(
	'charset',
	'Character set',
	'text',
	$pconfig['charset']
))->setHelp('Tickets are generated with the specified character set. It should contain printable characters (numbers, lower case and upper case letters) that are hard to confuse with others. Avoid e.g. 0/O and l/1.');

$section->addInput(new Form_Input(
	'rollbits',
	'# of Roll bits',
	'text',
	$pconfig['rollbits']
))->setHelp('Reserves a range in each voucher to store the Roll # it belongs to. Allowed range: 1..31. Sum of Roll+Ticket+Checksum bits must be one Bit less than the RSA key size.');

$section->addInput(new Form_Input(
	'ticketbits',
	'# of Ticket bits',
	'text',
	$pconfig['ticketbits']
))->setHelp('Reserves a range in each voucher to store the Ticket# it belongs to. Allowed range: 1..16. ' .
					'Using 16 bits allows a roll to have up to 65535 vouchers. ' .
					'A bit array, stored in RAM and in the config, is used to mark if a voucher has been used. A bit array for 65535 vouchers requires 8 KB of storage. ');

$section->addInput(new Form_Input(
	'checksumbits',
	'# of Checksum bits',
	'text',
	$pconfig['checksumbits']
))->setHelp('Reserves a range in each voucher to store a simple checksum over Roll # and Ticket#. Allowed range is 0..31.');

$section->addInput(new Form_Input(
	'magic',
	'Magic number',
	'text',
	$pconfig['magic']
))->setHelp('Magic number stored in every voucher. Verified during voucher check. ' .
					'Size depends on how many bits are left by Roll+Ticket+Checksum bits. If all bits are used, no magic number will be used and checked.');

$section->addInput(new Form_Input(
	'msgnoaccess',
	'Invalid voucher message',
	'text',
	$pconfig['msgnoaccess']
))->setHelp('Error message displayed for invalid vouchers on captive portal error page ($PORTAL_MESSAGE$).');


$section->addInput(new Form_Input(
	'msgexpired',
	'Expired voucher message',
	'text',
	$pconfig['msgexpired']
))->setHelp('Error message displayed for expired vouchers on captive portal error page ($PORTAL_MESSAGE$).');

$form->addGlobal(new Form_Input(
	'zone',
	null,
	'hidden',
	$cpzone
));

$form->addGlobal(new Form_Input(
	'exponent',
	null,
	'hidden',
	$pconfig['exponent']
));

$form->add($section);
print($form);
?>

<script type="text/javascript">
//<![CDATA[
events.push(function() {

	// Hides all elements of the specified class. This will usually be a section or group
	function hideClass(s_class, hide) {
		if (hide) {
			$('.' + s_class).hide();
		} else {
			$('.' + s_class).show();
		}
	}

	function setShowHide (show) {
		hideClass('rolledit', !show);

		if (show) {
			$('td:nth-child(5),th:nth-child(5)').show();
		} else {
			$('td:nth-child(5),th:nth-child(5)').hide();
		}
	}

	// Show/hide on checkbox change
	$('#enable').click(function() {
		setShowHide($('#enable').is(":checked"));
	})

	// Set initial state
	setShowHide($('#enable').is(":checked"));

	var generateButton = $('<a class="btn btn-xs btn-warning"><i class="fa-solid fa-arrows-rotate icon-embed-btn"></i><?=gettext("Generate new keys");?></a>');
	generateButton.on('click', function() {
		$.ajax({
			type: 'post',
			url: 'services_captiveportal_vouchers.php',
			data: {
				generatekey:           true,
			},
			dataType: 'json',
			success: function(data) {
				$('#publickey').val(data.public.replace(/\\n/g, '\n'));
				$('#privatekey').val(data.private.replace(/\\n/g, '\n'));
			}
		});
	});
	generateButton.appendTo($('#publickey + .help-block')[0]);
});
//]]>
</script>
<?php include("foot.inc");
