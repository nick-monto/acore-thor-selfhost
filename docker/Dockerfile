# Container image for this realm: the published server artifact, the handful of runtime packages
# its binaries need, and the entrypoint that turns it into a running realm.
#
# The build context is the repository root (docker/docker-compose.yml builds with `context: ..`),
# which is what lets this file COPY docker/entrypoint.sh and scripts/write-confs.sh. .dockerignore
# keeps that context down to those two files plus this directory.
#
# Why the image is thin: the payload is the prebuilt artifact, not a compile. The core takes hours
# to build, and the same tarball is what the Thor installer and the Windows installer unpack, so a
# rebuild here is a download - and the container runs exactly the binaries those hosts run.
#
# Why the client data is NOT in the image: dbc/ and maps/ are about 1.2 GiB. Baked in, they would
# be a layer that every rebuild re-downloads and every data change invalidates. They live in the
# `data` volume instead and docker/entrypoint.sh fetches them on first start.

FROM ubuntu:24.04

# BuildKit sets TARGETARCH to the platform being built for. The classic builder leaves it empty, in
# which case dpkg's architecture is the same answer.
ARG TARGETARCH

# Overrides. ARTIFACT_URL points the build at another tarball (a local release, a fork's build) and
# ARTIFACT_SHA256 pins it. When neither the arg nor the `.sha256` published beside the URL supplies
# a hash, the build stops rather than unpacking bytes nobody verified.
ARG ARTIFACT_URL=
ARG ARTIFACT_SHA256=

ENV DEBIAN_FRONTEND=noninteractive

# Runtime only: no compilers, no headers, no git, because nothing in this image builds anything.
#   mysql-client-8.0  the core shells out to `mysql` for Updates.EnableDatabases, and the
#                     entrypoint uses `mysql`/`mysqladmin`. mysql-client-core-8.0 behind it carries
#                     both binaries. The concrete package is named rather than the `mysql-client`
#                     metapackage, which follows whatever server major Ubuntu ships next: the same
#                     generation as the libmysqlclient.so.21 the artifact bundles, and an image
#                     that does not change its client under a rebuild.
#   unzip             the client data ships as data.zip.
#   libreadline8t64   worldserver links libreadline for its console even with Console.Enable=0,
#                     and ubuntu:24.04's base layer does not carry it. The base layer does carry
#                     everything else the binaries need that the artifact does not bundle itself:
#                     libc, libstdc++6, libssl3t64 (pulled in by the MySQL client), libbz2,
#                     liblzma and libzstd. MySQL's client library and ICU travel in the artifact's
#                     lib/ and are found through the $ORIGIN/../lib rpath the build sets.
#                     python3 is for scripts/item-dbc/patch_item_dbc.py, which the entrypoint runs
#                     before every start so the server's Item.dbc carries the loot-bag records the
#                     same start adds to item_template. It is the full package, not -minimal: the
#                     tool imports difflib through the loot-bag generator, and -minimal's curated
#                     stdlib does not have it - the first build of this step failed on exactly that
#                     and warned instead of patching. It is the only interpreter here; nothing builds.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        python3 \
        zstd \
        tzdata \
        unzip \
        libreadline8t64 \
        mysql-client-8.0 \
 && rm -rf /var/lib/apt/lists/*

# The artifact, verified before it is unpacked. The tarball's top level is bin/ etc/ lib/ src/ for
# prefix /opt/acore, so it is extracted straight into /opt/acore with nothing to strip.
RUN set -eu; \
    arch="${TARGETARCH:-$(dpkg --print-architecture)}"; \
    case "$arch" in \
        amd64) default_url="https://github.com/nick-monto/acore-thor-selfhost/releases/download/server-build-x86_64/acore-server-x86_64.tar.zst" ;; \
        arm64) default_url="https://github.com/nick-monto/acore-thor-selfhost/releases/download/server-build/acore-server-arm64.tar.zst" ;; \
        *) echo "unsupported TARGETARCH '$arch': the release publishes x86_64 and arm64 artifacts only" >&2; exit 1 ;; \
    esac; \
    url="${ARTIFACT_URL:-$default_url}"; \
    sha="$ARTIFACT_SHA256"; \
    if [ -z "$sha" ]; then \
        if curl -fsSL --retry 3 -o /tmp/artifact.sha256 "$url.sha256"; then \
            sha="$(awk 'NR == 1 { print $1 }' /tmp/artifact.sha256)"; \
        fi; \
    fi; \
    if [ -z "$sha" ]; then \
        echo "no sha256 for $url: pass --build-arg ARTIFACT_SHA256=... (the workflows publish one beside the asset)" >&2; \
        exit 1; \
    fi; \
    echo "downloading $url"; \
    curl -fL --retry 3 -o /tmp/artifact.tar.zst "$url"; \
    echo "$sha  /tmp/artifact.tar.zst" | sha256sum -c -; \
    mkdir -p /opt/acore; \
    tar --zstd -xf /tmp/artifact.tar.zst -C /opt/acore; \
    rm -f /tmp/artifact.tar.zst /tmp/artifact.sha256; \
    for dir in bin etc lib src; do \
        if [ ! -d "/opt/acore/$dir" ]; then \
            echo "the artifact has no $dir/ at its top level; is $url the server tarball?" >&2; \
            exit 1; \
        fi; \
    done; \
    test -x /opt/acore/bin/worldserver; \
    test -x /opt/acore/bin/authserver

COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY scripts/write-confs.sh /opt/acore/write-confs.sh
# The Item.dbc patcher and the generator it imports for the bag records. The layout is the
# repository's own (item-dbc/ beside loot-bags/), because the tool loads its sibling by path.
COPY scripts/item-dbc/patch_item_dbc.py /opt/acore/dbc-patch/item-dbc/patch_item_dbc.py
COPY scripts/loot-bags/gen_loot_bags.py /opt/acore/dbc-patch/loot-bags/gen_loot_bags.py

# The exec bit comes from the checkout, and a checkout made on Windows can lose it; the entrypoint
# is what the container runs, so set it rather than trust the copy.
RUN chmod 0755 /usr/local/bin/entrypoint.sh

WORKDIR /opt/acore

# 3724 auth, 8085 world. The database is reached over the compose network, not published.
EXPOSE 3724 8085

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
