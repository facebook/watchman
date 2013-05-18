<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

/* Loads a data file produced by:
 * `valgrind --tool=callgrind --collect-jumps=yes`
 * and parses out information on which files were used
 * and which lines were executed
 * http://valgrind.org/docs/manual/cl-format.html
 */

class CallgrindFile {
  public $filename;
  public $names = array();
  public $function_costs = array();

  private $name_map = array();
  private $files_touched = array();

  function __construct($filename) {
    $this->filename = $filename;
  }

  function parse() {
    $fp = fopen($this->filename, 'r');

    $props = array();
    // which column holds the line number
    $line_position = 0;

    $cg_file_line_number = 0;
    $line = null;
    $current_fn_line = 0;
    $current_file_name = null;
    while (true) {
      $last_line = $line;
      $cg_file_line_number++;
      $line = fgets($fp);
      if ($line === false) break;
      $line = rtrim($line, "\r\n");
      if (!strlen($line)) continue;

      if (preg_match('/^#/', $line)) {
        continue;
      }

      // key: value
      if (preg_match("/^([a-z]+):\s*(.*)\s*$/", $line, $M)) {
        $key = $M[1];
        $val = $M[2];
        $props[$key] = $val;

        if ($key == 'positions') {
          $positions = explode(' ', $val);
          $line_position = array_search('line', $positions);
        }
        continue;
      }

      // Position = (Number) Name
      if (preg_match("/^[cj]?f[ile]=\s*\(([^)]+)\)\s+(\S+)$/",
          $line, $M)) {
        // file name for the subsequent cost lines

        list($_, $id, $name) = $M;
        $this->name_map[$id] = $name;
        if ($line[0] == 'f') {
          if (!isset($this->files_touched[$name])) {
            $this->files_touched[$name] = array();
          }
          $current_file_name = $name;
        }
        continue;
      }

      // Position = Name
      if (preg_match("/^(fl|fi|fe)=\s*\((\d+)\)$/", $line, $M)) {
        $id = $M[2];
        if (!isset($this->name_map[$id])) {
          throw new Exception(
            "wanted id $id but we haven't seen it at $cg_file_line_number"
          );
        }
        $current_file_name = $this->name_map[$id];
        continue;
      }

      // Position = Name
      if (preg_match("/^(fl|fi|fe)=\s*(\S+)$/", $line, $M)) {
        $name = $M[2];
        if (!isset($this->files_touched[$name])) {
          $this->files_touched[$name] = array();
        }
        $current_file_name = $name;
        continue;
      }

      // AssociationSpecification
      if (preg_match('/^calls=\s*(\S+)\s+(.*)$/', $line, $M)) {
        $last_assoc = $M;
        $line = rtrim(fgets($fp), "\r\n");
        $cg_file_line_number++;
        continue;
      }

      if (preg_match('/^(ob|fn|jcnd|cob|cfi|cfn|jump|jfi)=/', $line)) {
        // Explicitly ignore
        continue;
      }

      if (preg_match('/^[a-z]+=/', $line)) {
        throw new Exception("unhandled $line");
      }

      $fields = $this->parsePosList($line, $current_fn_line);
      if ($fields[$line_position] < 0) {
        printf("Line number ended up negative: %d to start\n",
          $current_fn_line);
        print_r($fields);
        printf("line was: %s\n", $line);
        throw new Exception("suck at line $cg_file_line_number");
      }
      $current_fn_line = $fields[$line_position];
      $this->files_touched[$current_file_name][$current_fn_line] = true;
    }
  }

  // parses a position list and turns it into the same sequence of
  // absolute numbers
  private function parsePosList($line, $base) {
    $res = array();
    $fields = preg_split('/\s+/', trim($line));
    foreach ($fields as $field) {
      switch ($field[0]) {
        case '+':
          $res[] = $base + substr($field, 1);
          break;
        case '-':
          $res[] = $base - substr($field, 1);
          break;
        case '*':
          $res[] = $base;
          break;
        default:
          if (preg_match('/^0x/', $field)) {
            $res[] = hexdec($field);
          } else {
            $res[] = (int)$field;
          }
      }
    }
    return $res;
  }

  static function max(array $a) {
    if (!count($a)) {
      return 0;
    }
    return max($a);
  }

  static function computeCoverageString($sourcefile, array $touched_lines) {
    $exe_lines = DwarfLineInfo::mergedSourceLines($sourcefile);
    if (!count($exe_lines)) {
      return "";
    }
    $max_line = max(self::max($exe_lines), self::max($touched_lines));

    /* now build a string containing the coverage info.
     * This is compatible with ArcanistUnitTestResult coverage data:
     * N not executable
     * C covered
     * U uncovered
     * X unreachable (we can't detect that here)
     */
    $cov = str_repeat('N', $max_line);
    for ($i = 1; $i <= $max_line; $i++) {
      if (isset($touched_lines[$i])) {
        $cov[$i - 1] = 'C';
      } else if (isset($exe_lines[$i])) {
        $cov[$i - 1] = 'U';
      }
    }

    return $cov;
  }

  static function mergeSourceLineData($sourcefile, array $cg_list) {
    $lines = array();
    foreach ($cg_list as $cg) {
      $cg->getTouchedLines($sourcefile, $lines);
    }

    return self::computeCoverageString($sourcefile, $lines);
  }

  function getTouchedLines($sourcefile, &$all_lines) {
    foreach (idx($this->files_touched, $sourcefile, array()) as $no => $_) {
      $all_lines[$no] = $no;
    }
  }

  function collectSourceFileData($sourcefile) {
    $all_lines = array();
    $this->getTouchedLines($sourcefile, $all_lines);

    foreach ($this->getObjectFiles() as $objfile) {
      DwarfLineInfo::loadObject($objfile);
    }
    $exe_lines = DwarfLineInfo::mergedSourceLines($sourcefile);
    $max_line = max($exe_lines);

    /* now build a string containing the coverage info.
     * This is compatible with ArcanistUnitTestResult coverage data:
     * N not executable
     * C covered
     * U uncovered
     * X unreachable (we can't detect that here)
     */
    $cov = str_repeat('N', $max_line);
    for ($i = 1; $i <= $max_line; $i++) {
      if (isset($all_lines[$i])) {
        $cov[$i - 1] = 'C';
      } else if (isset($exe_lines[$i])) {
        $cov[$i - 1] = 'U';
      }
    }

    return $cov;
  }

  /* print a source file with coverage annotation */
  function annotate($sourcefile) {
    $cov = $this->collectSourceFileData($sourcefile);
    $fp = fopen($sourcefile, 'r');
    $lineno = 0;
    while (true) {
      $line = fgets($fp);
      if ($line === false) break;
      $lineno++;
      $line = rtrim($line, "\r\n");

      $sigil = 'N';
      if (isset($cov[$lineno - 1])) {
        $sigil = $cov[$lineno - 1];
      }

      printf("%s %s\n", $sigil, $line);
    }
  }

  function getSourceFiles() {
    return array_keys($this->files_touched);
  }
}



// vim:ts=2:sw=2:et:

