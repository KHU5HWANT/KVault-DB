FROM ubuntu:24.04

# Avoid tzdata interactive prompt during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies (Using clang instead of gcc to save massive amounts of RAM during compilation)
RUN apt-get update && apt-get install -y \
    clang \
    cmake \
    ninja-build \
    git \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set Clang as the default compiler to prevent OOM
ENV CC=clang
ENV CXX=clang++

# Set the working directory
WORKDIR /app

# Copy the project files
COPY . .

# Configure and Build the project (MinSizeRel uses -Os which drastically reduces compiler RAM usage compared to Release -O3)
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF
# Run strictly sequential build (-j 1) to prevent Render's 512MB RAM limit from triggering the OOM killer
RUN cmake --build build -j 1 --target kvault_server

# Render expects web services to listen on port 8080 by default (or via PORT env var)
EXPOSE 8080

# Ensure data directories exist
RUN mkdir -p data/sstables data/wal

# Set the entrypoint to the compiled C++ binary
CMD ["./build/kvault_server"]
