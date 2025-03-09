FROM ubuntu:24.04

ARG USERNAME=dev
ARG USER_UID=1001
ARG USER_GID=$USER_UID

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive

# Install essential dependencies
RUN apt-get update && apt-get install -y \
  apt-utils \
  build-essential \
  cmake \
  curl \
  git \
  gnupg \
  libncurses5-dev \
  libncursesw5-dev \
  libssl-dev \
  flex \
  bison \
  lsb-release \
  ninja-build \
  pkg-config \
  python3 \
  python3-pip \
  sudo \
  tar \
  unzip \
  wget \
  zip \
  googletest \
  libpqxx-dev \
  libboost-all-dev \
  ca-certificates \
  clang-18 \
  clang-format-18 \
  clang-tidy-18 \
  lldb


# && apt-get clean \
# && rm -rf /var/lib/apt/lists/*

# Create the user
RUN groupadd --gid $USER_GID $USERNAME || echo "Group with GID $USER_GID already exists" \
  && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME || (groupmod -g 1001 $(getent group $USER_GID | cut -d: -f1) \
  && groupadd --gid $USER_GID $USERNAME \
  && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME) \
  && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
  && chmod 0440 /etc/sudoers.d/$USERNAME \
  && (groupadd -g 999 docker || echo "Docker group already exists") \
  && usermod -aG docker $USERNAME

# # Install neovim
# RUN curl -LO https://github.com/neovim/neovim/releases/latest/download/nvim-linux64.tar.gz \
#   && tar -xzf nvim-linux64.tar.gz -C /opt \
#   && ln -s /opt/nvim-linux64/bin/nvim /usr/local/bin/nvim \
#   && rm nvim-linux64.tar.gz

# Install vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg \
  && /opt/vcpkg/bootstrap-vcpkg.sh \
  && ln -s /opt/vcpkg/vcpkg /usr/local/bin/vcpkg \
  && chown -R $USERNAME:$USERNAME /opt/vcpkg

# Configure vcpkg for user
ENV VCPKG_ROOT=/opt/vcpkg

# # Install node.js (required for some LSP features)
# RUN curl -fsSL https://deb.nodesource.com/setup_18.x | bash - \
#   && apt-get install -y nodejs \
#   && apt-get clean \
#   && rm -rf /var/lib/apt/lists/*

# # Install ripgrep (used by many Neovim plugins)
# RUN curl -LO https://github.com/BurntSushi/ripgrep/releases/download/13.0.0/ripgrep_13.0.0_amd64.deb \
#   && dpkg -i ripgrep_13.0.0_amd64.deb \
#   && rm ripgrep_13.0.0_amd64.deb

# Set the default user
USER $USERNAME
WORKDIR /home/$USERNAME

# # Install AstroNvim (optional - comment out if you prefer to use your own config)
# RUN git clone --depth 1 https://github.com/AstroNvim/AstroNvim ~/.config/nvim

# Add custom bash configuration
RUN echo 'export PATH=$PATH:/opt/vcpkg' >> ~/.bashrc \
  && echo 'alias ll="ls -la"' >> ~/.bashrc

# Installing Apache Arrow from VCPKG
RUN vcpkg install arrow \
  && vcpkg install nlohmann-json

# Define volumes for persistent data
VOLUME ["/home/$USERNAME/workspace"]

# Set the user
USER $USERNAME
