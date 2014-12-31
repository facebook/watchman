<?php
array_shift($argv);
$log_file_name = array_shift($argv);
$out = fopen($log_file_name, 'a');
foreach ($argv as $arg) {
  fprintf($out, "%d %s\n", time(), $arg);
}
fclose($out);
printf("WOOT from trig.sh\n");
