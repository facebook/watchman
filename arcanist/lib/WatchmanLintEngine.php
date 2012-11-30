<?php
// Copyright 2004-present Facebook.  All rights reserved.

class WatchmanLintEngine extends ArcanistLintEngine {

  public function buildLinters() {
    $linters = array();
    $paths = $this->getPaths();

    // Remove all deleted files, which are not checked by the
    // following linters.
    foreach ($paths as $key => $path) {
      if (!Filesystem::pathExists($this->getFilePathOnDisk($path))) {
        unset($paths[$key]);
      }
    }

    $generated_linter = new ArcanistGeneratedLinter();
    $linters[] = $generated_linter;

    $nolint_linter = new ArcanistNoLintLinter();
    $linters[] = $nolint_linter;

    $apache_license_linter = new ArcanistApacheLicenseLinter();
    $linters[] = $apache_license_linter;

    $text_linter = new ArcanistTextLinter();
    $text_linter->setCustomSeverityMap(array(
      ArcanistTextLinter::LINT_LINE_WRAP
        => ArcanistLintSeverity::SEVERITY_ADVICE,
    ));
    $linters[] = $text_linter;

    $spelling_linter = new ArcanistSpellingLinter();
    $linters[] = $spelling_linter;

    foreach ($paths as $path) {
      if (preg_match('/\.(c|php|markdown|h)$/', $path)) {
        $nolint_linter->addPath($path);

        $generated_linter->addPath($path);
        $generated_linter->addData($path, $this->loadData($path));

        $text_linter->addPath($path);
        $text_linter->addData($path, $this->loadData($path));

        $spelling_linter->addPath($path);
        $spelling_linter->addData($path, $this->loadData($path));
      }
      if (preg_match('/\.[ch]$/', $path)) {
        $apache_license_linter->addPath($path);
        $apache_license_linter->addData($path, $this->loadData($path));
      }
    }

    $name_linter = new ArcanistFilenameLinter();
    $linters[] = $name_linter;
    foreach ($paths as $path) {
      $name_linter->addPath($path);
    }

    return $linters;
  }
}
