FROM quay.io/pypa/manylinux_2_28_x86_64
ENV GCC_TOOLSET=/opt/rh/gcc-toolset-14
RUN dnf install -y gcc-toolset-14 git pkgconf-pkg-config postgresql-devel libcurl-devel openssl-devel \
        wget ca-certificates && dnf clean all
# Apache Arrow for AlmaLinux 8, then EPEL for Eigen and NLopt
RUN dnf install -y epel-release && \
    dnf install -y https://packages.apache.org/artifactory/arrow/almalinux/8/apache-arrow-release-latest.rpm && \
    dnf install -y arrow-devel eigen3-devel && dnf clean all
ENV PATH=/opt/rh/gcc-toolset-14/root/usr/bin:$PATH
ENV LD_LIBRARY_PATH=/opt/rh/gcc-toolset-14/root/usr/lib64:$LD_LIBRARY_PATH
RUN git clone --depth 1 https://github.com/nlohmann/json.git /tmp/json && \
    cmake -S /tmp/json -B /tmp/json/build -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DJSON_BuildTests=OFF && \
    cmake --build /tmp/json/build --target install && rm -rf /tmp/json
RUN git clone --branch 7.10.4 --depth 1 https://github.com/jtv/libpqxx.git /tmp/libpqxx && \
    cmake -S /tmp/libpqxx -B /tmp/libpqxx/build -DSKIP_BUILD_TEST=ON -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    cmake --build /tmp/libpqxx/build -j"$(nproc)" --target install && rm -rf /tmp/libpqxx && ldconfig
RUN git clone --branch v2.10.0 --depth 1 https://github.com/stevengj/nlopt.git /tmp/nlopt && \
    cmake -S /tmp/nlopt -B /tmp/nlopt/build -DNLOPT_PYTHON=OFF -DNLOPT_OCTAVE=OFF -DNLOPT_MATLAB=OFF \
        -DNLOPT_GUILE=OFF -DNLOPT_SWIG=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    cmake --build /tmp/nlopt/build -j"$(nproc)" --target install && rm -rf /tmp/nlopt && ldconfig
RUN /opt/python/cp312-cp312/bin/pip install -q auditwheel scikit-build-core ninja cmake patchelf
RUN dnf remove -y eigen3-devel && git clone --branch 3.4.0 --depth 1 https://gitlab.com/libeigen/eigen.git /tmp/eigen && \
    cmake -S /tmp/eigen -B /tmp/eigen/build -DBUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF && \
    cmake --build /tmp/eigen/build --target install && rm -rf /tmp/eigen
WORKDIR /src




