### Set up

1. Download docker. https://www.docker.com/products/docker-desktop/

2. Open Docker -- Docker must be open to activate the docker engine for building applications

### Building and running your application

When you're ready, start your application by running:
```bash
docker build -t trade_ngin .
```

Enter the container's terminal:
```bash
docker run -it --entrypoint /bin/bash trade_ngin`
```

Run run your build:
```bash
./start.up
```

### Warning:

At times (for reasons unknown to the writer), docker fails to import generic libraries from source code. If you run the dockerfile, you might be met with an extensive error.

Example of an error:
```bash
/app/include/trade_ngin/order/order_manager.hpp:83:38: error: variable 'std::atomic<long unsigned int> counter' has initializer but incomplete type
    83 |         static std::atomic<uint64_t> counter{0};
       |                                      ^~~~~~~
```

This error complains that it does not know what type 'counter{0}' is, but it is defined within the Atomic library and _is_ included in the /order_manager.hpp file. In order to fix this error, use 'sed' (a Unix utility short for stream editor) to import the library manually.

Fix
```Dockerfile
RUN sed -i '1i\#include <atomic>' include/trade_ngin/order/order_manager.hpp
```

Please add this code near line 70 in the dockerfile. Notice '1i\' preceeds '#include <atomic>.' This is necessary and tells docker to add '#include <atomic>' before the first line in the file. Also include the relative file path afterwards.
