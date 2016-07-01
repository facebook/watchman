/*
 * Copyright 2015-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
package com.facebook.watchman.util;

import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Arrays;

import org.junit.rules.ExternalResource;

public class TemporaryPaths extends ExternalResource {

  private final boolean keepContents;
  private Path root;

  public TemporaryPaths() {
    this(false);
  }

  public TemporaryPaths(boolean keepContents) {
    this.keepContents = keepContents;
  }

  @Override
  protected void before() throws Throwable {
    root = Files.createTempDirectory("junit-temp-path").toRealPath();
  }

  public Path getRoot() {
    return root;
  }

  public Path newFolder() throws IOException {
    return Files.createTempDirectory(root, "tmpFolder");
  }

  @Override
  @SuppressWarnings("PMD.EmptyCatchBlock")
  protected void after() {
    if (root == null) {
      return;
    }

    if (keepContents) {
      System.out.printf("Contents available at %s.\n", getRoot());
      return;
    }

    try {
      Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
        @Override
        public FileVisitResult visitFile(
            Path file, BasicFileAttributes attrs) throws IOException {
          Files.delete(file);
          return FileVisitResult.CONTINUE;
        }

        @Override
        public FileVisitResult postVisitDirectory(Path dir,
            IOException exc) throws IOException {
          Files.delete(dir);
          return FileVisitResult.CONTINUE;
        }
      });
    } catch (IOException e) {
      // Swallow. Nothing sane to do.
    }
  }

  public Path newFile(String fileName) throws IOException {
    Path toCreate = root.resolve(fileName);

    if (Files.exists(toCreate)) {
      throw new IOException(
          "a file with the name \'" + fileName + "\' already exists in the test folder");
    }

    return Files.createFile(toCreate);
  }

  public Path newFile() throws IOException {
    return Files.createTempFile(root, "junit", "file");
  }

  public Path newFolder(String... name) throws IOException {
    Path toCreate = root;
    for (String segment : name) {
      toCreate = toCreate.resolve(segment);
    }

    if (Files.exists(toCreate)) {
      throw new IOException(
          String.format(
              "a folder with the name '%s' already exists in the test folder",
              Arrays.toString(name)));
    }

    return Files.createDirectories(toCreate);
  }
}