<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanLicenseLinter extends ArcanistLinter {
  const LINT_NO_LICENSE_HEADER = 1;
  const LINT_WRONG_LICENSE_HEADER = 2;

  public function willLintPaths(array $paths) {
    return;
  }

  public function getLintSeverityMap() {
    return array();
  }

  public function getLintNameMap() {
    return array(
      self::LINT_NO_LICENSE_HEADER => 'No License Header',
      self::LINT_WRONG_LICENSE_HEADER => 'Invalid License Header',
    );
  }

  public function getLinterName() {
    return 'LICENSE';
  }

  public function lintPath($path) {
    if (preg_match('/^(python|ruby)/', $path)) {
      return;
    }
    $source = $this->getData($path);

    $template = <<<TXT
/* Copyright DATE Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */


TXT;

    $pattern = '!' .
      str_replace("DATE", "\d{4}-present", preg_quote(trim($template))) .
      '!';

    $maybe_php_or_script = '(#![^\n]+?[\n])?(<[?]php\s+?)?';
    $find = array(
      "@^{$maybe_php_or_script}//[^\n]*Copyright[^\n]*[\n]\s*@i",
      "@^{$maybe_php_or_script}/[*](?:[^*]|[*][^/])*?Copyright.*?[*]/\s*@is",
      "@^{$maybe_php_or_script}\s*@",
    );
    foreach ($find as $pat) {
      if (preg_match($pat, $source, $M)) {
        $got = trim($M[0]);

        if (preg_match($pattern, $got)) {
          // Good enough
          break;
        }

        // Try to find starting year
        if (preg_match('/Copyright\s+(\d{4})/', $got, $matches)) {
          $year = $matches[1];
        } else {
          $year = date('Y');
        }
        $suggest =
          ltrim(rtrim(implode('', array_slice($M, 1))) . "\n" .
          str_replace("DATE", $year . '-present', $template));

        if (!strlen($got)) {
          $this->raiseLintAtOffset(
            0,
            self::LINT_NO_LICENSE_HEADER,
            'This file is missing a license header.',
            $M[0],
            $suggest);
          break;
        }
        break;
      }
    }
  }
}

// vim:ts=2:sw=2:et:
