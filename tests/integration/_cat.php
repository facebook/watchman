<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
array_shift($argv);
$files = $argv;
if (!count($files)) {
  $files[] = '-';
}
foreach ($files as $file) {
  if ($file == '-') {
    stream_copy_to_stream(STDIN, STDOUT);
  } else {
    $fp = fopen($file, 'rb');
    stream_copy_to_stream($fp, STDOUT);
    fclose($file);
  }
}
