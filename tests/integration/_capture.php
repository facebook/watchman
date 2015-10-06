<?php
$out = $argv[1];
$env = $argv[2];

// Emulate the behavior of the shell
$_ENV['PWD'] = getcwd();

printf("_capture.php: opening $out and sending stdin to it\n");
$out_fp = fopen($out, 'w');
$input = stream_get_contents(STDIN);
printf("input is:\n%s\n", $input);
fprintf($out_fp, "%s\n", $input);
fclose($out_fp);

printf("_capture.php: opening $env and sending env to it\n");
$env_fp = fopen($env, 'w');
foreach ($_ENV as $k => $v) {
  printf("%s", "$k=$v\n");
  fprintf($env_fp, "%s", "$k=$v\n");
}
fclose($env_fp);
