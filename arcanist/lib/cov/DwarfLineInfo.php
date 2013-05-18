<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

/* Uses `dwarfdump` to determine which lines are executable in
 * a given object file or set of object files.
 * No need to parse the sources.
 */

class DwarfLineInfo {
  public $filename;
  public $lines_by_file = array();
  static $cache = array();

  static function loadObject($filename) {
    $filename = realpath($filename);
    if (isset(self::$cache[$filename])) {
      return self::$cache[$filename];
    }
    $D = new DwarfLineInfo($filename);
    $D->parse();
    self::$cache[$filename] = $D;
    return $D;
  }

  static function mergedSourceLines($filename) {
    $filename = realpath($filename);
    $lines = array();
    foreach (self::$cache as $D) {
      if (!isset($D->lines_by_file[$filename])) continue;
      foreach ($D->lines_by_file[$filename] as $line) {
        $lines[$line] = $line;
      }
    }
    return $lines;
  }

  function __construct($filename) {
    $this->filename = $filename;
  }

  function parse() {
    $fp = popen(sprintf("dwarfdump -l %s",
            escapeshellarg($this->filename)), 'r');

    $sourcefile = null;
    while (true) {
      $line = fgets($fp);
      if ($line === false) break;

      // 0x004021d0  [  53, 0] NS uri: "/path/to/file.c"
      if (preg_match("/^0x[0-9a-fA-F]+\s+\[\s*(\d+),\s*\d+\]/", $line, $M)) {
        $exe_line = $M[1];

        if (preg_match('/uri: "([^"]+)"/', $line, $M)) {
          $sourcefile = realpath($M[1]);


          if (!isset($this->lines_by_file[$sourcefile])) {
            $this->lines_by_file[$sourcefile] = array();
          }
        }
        $this->lines_by_file[$sourcefile][$exe_line] = $exe_line;
      }
    }
  }
}

// vim:ts=2:sw=2:et:
