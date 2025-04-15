FROM ubuntu:22.04
RUN apt-get update && apt-get install -y build-essential cmake valgrind linux-perf gdb
RUN apt-get install -y g++
RUN useradd -m -s /bin/bash runner
WORKDIR /app
COPY . /app/
RUN chown -R runner:runner /app
USER runner
WORKDIR /app/build/bin
RUN chmod +x /app/build/bin/run_valgrind.sh /app/build/bin/run_tsan.sh /app/build/bin/run_perf.sh
ENTRYPOINT ["/bin/bash"] 