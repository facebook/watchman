FROM gcc

ADD . /usr/src/watchman/
WORKDIR /usr/src/watchman/

RUN \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive \
    apt-get -y install \
      python-dev \
  && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/ \
  && \
  ./autogen.sh && \
  ./configure && \
  make && \
  make install

ENTRYPOINT [ "watchman" ]
CMD [ "--help" ]
