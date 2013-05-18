<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanCoverWorkflow extends ArcanistBaseWorkflow {
  public function getWorkflowName() {
    return 'cov';
  }

  public function getCommandSynopses() {
    return phutil_console_format(<<<TXT
      **cov** [file]
TXT
    );
  }

  public function getCommandHelp() {
    return phutil_console_format(<<<TXT
          Shows coverage data from prior test run
TXT
    );
  }

  public function requiresConduit() {
    return false;
  }

  public function requiresRepositoryAPI() {
    return true;
  }

  public function requiresAuthentication() {
    return false;
  }

  public function getArguments() {
    return array(
      '*' => 'files',
    );
  }

  public function run() {
    $cov = $this->readScratchJSONFile('wman-cov.json');

    // Order the cov data from best to worst coverage, so that
    // repeated `arc cov` runs will always show the best target
    // without needing to scroll back
    $cov_score = array();
    foreach ($cov as $filename => $lines) {
      $u = 0;
      $c = 0;
      $l = strlen($lines);
      for ($i = 0; $i < $l; $i++) {
        if ($lines[$i] == 'C') {
          $c++;
        } else if ($lines[$i] == 'U') {
          $u++;
        }
      }
      $cov_score[$filename] = $c / ($u + $c);
    }
    arsort($cov_score);

    $files = $this->getArgument('files');
    if (!$files) {
      $files = array_keys($cov_score);
    }
    $files = array_flip($files);

    foreach ($cov_score as $filename => $score) {
      if (!isset($files[$filename])) {
        continue;
      }
      $lines = $cov[$filename];
      if (strpos($lines, 'U') === false) {
        // No uncovered lines
        continue;
      }
      printf("\nCoverage for %s\n", $filename);
      $source_lines = file($filename);

      $i = 0;
      $l = strlen($lines);
      $last = -100;
      for ($i = 0; $i < $l; $i++) {
        if ($lines[$i] == 'U') {
          if ($last != $i - 1) {
            printf("...\n");
          }
          printf("%6d  %s", $i, $source_lines[$i]);
          $last = $i;
        }
      }
      printf("%s coverage: %.2f%%\n", $filename, $score * 100);
    }

    return 0;
  }
}

// vim:ts=2:sw=2:et:

