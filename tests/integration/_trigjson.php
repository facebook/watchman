<?php
# $1 = name of a log file
# the rest of the args are names of files that were modified
$log = $argv[1];
# Copy json from stdin to the log file
$fp = fopen($log, 'a');
printf("_trigjson.php: Copying STDIN to $log\n");
$in = stream_get_contents(STDIN);
printf("stdin: %s\n", $in);
fwrite($fp, $in);
fclose($fp);
