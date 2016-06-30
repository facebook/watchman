Watchman Java Library
====

This provides Java bindings to the Watchman service.

Building
===

Make sure that you have [buck](https://buckbuild.com/) installed. In this
folder, run:

```
buck build :watchman
```

The resulting JAR file is found in: 
`buck-out/gen/lib__watchman__output/watchman.jar`

To run the tests:

```
buck test :watchman
```

