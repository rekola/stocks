# Third-party licenses

stocks fetches its dependencies at pinned versions when it is built (see `CMakeLists.txt`). This file keeps the licences of
those that need their notice to travel with the program; it starts with the ones that are built into it. (immer, nlohmann/json,
httpclient and cellui are fetched too and are not listed here yet.)

## SQLiteCpp

MIT licence. <https://github.com/SRombauts/SQLiteCpp>, version 3.4.0. Keeps the node attributes between runs
(`src/model/SqliteFactStore.cpp`).

```
The MIT License (MIT)

Copyright (c) 2012-2026 Sebastien Rombauts (sebastien.rombauts@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## SQLite

Public domain. <https://www.sqlite.org>. It is built from the amalgamation bundled with SQLiteCpp (`SQLITECPP_INTERNAL_SQLITE`),
so no system library is needed on any platform. The authors have dedicated the code to the public domain; the notice that
comes with it reads:

```
The author disclaims copyright to this source code.  In place of
a legal notice, here is a blessing:

   *  May you do good and not evil.
   *  May you find forgiveness for yourself and forgive others.
   *  May you share freely, never taking more than you give.
```

## pugixml

MIT licence. <https://github.com/zeux/pugixml>, version 1.14. Reads the Bank of Finland's XML report
(`src/source/SuomenPankkiSource.cpp`).

```
MIT License

Copyright (c) 2006-2023 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

## cpp-httplib

MIT licence, Copyright (c) 2017 yhirose. <https://github.com/yhirose/cpp-httplib>, version 0.18.3. The market-data server's HTTP
(`src/market/Api.cpp`); built only where the market-data programs are.
