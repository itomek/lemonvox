#!/usr/bin/env bash
set -euo pipefail
umask 022

if [[ $# -lt 3 ]]; then
    echo 'Usage: bash scripts/build-ubuntu-apt-repository.sh NEW_DIRECTORY SIGNING_FINGERPRINT PACKAGE.deb...' >&2
    exit 2
fi

output=$1
fingerprint=$2
shift 2
if [[ ! $fingerprint =~ ^([[:xdigit:]]{40}|[[:xdigit:]]{64})$ ]]; then
    echo 'Provide the full OpenPGP signing key fingerprint.' >&2
    exit 2
fi
if [[ -e $output || -L $output ]]; then
    echo 'Repository output must be a new directory.' >&2
    exit 2
fi

parent=$(dirname "$output")
mkdir -p "$parent"
stage=$(mktemp -d "$parent/.lemonvox-apt.XXXXXXXX")
trap 'rm -rf "$stage"' EXIT

pool=pool/main/l/lemonvox
mkdir -p "$stage/$pool"
declare -A architectures=()
for package in "$@"; do
    name=$(dpkg-deb -f "$package" Package)
    version=$(dpkg-deb -f "$package" Version)
    architecture=$(dpkg-deb -f "$package" Architecture)
    if [[ $name != lemonvox || ! $architecture =~ ^[a-z0-9]+$ || $architecture == all ]]; then
        echo "Expected an architecture-specific LemonVox package: $package" >&2
        exit 2
    fi
    dpkg --validate-version "$version"
    filename="lemonvox_${version}_${architecture}.deb"
    if [[ -e $stage/$pool/$filename ]]; then
        echo "Duplicate package: $filename" >&2
        exit 2
    fi
    install -m 644 "$package" "$stage/$pool/$filename"
    architectures["$architecture"]=1
done

(
    cd "$stage"
    mapfile -t sorted_architectures < <(printf '%s\n' "${!architectures[@]}" | LC_ALL=C sort)
    apt-ftparchive packages "$pool" > all-packages
    for architecture in "${sorted_architectures[@]}"; do
        index="dists/stable/main/binary-$architecture/Packages"
        mkdir -p "$(dirname "$index")"
        awk -v architecture="$architecture" '
            BEGIN { RS = ""; FS = "\n" }
            {
                for (i = 1; i <= NF; i++)
                    if ($i == "Architecture: " architecture)
                        printf "%s\n\n", $0
            }
        ' all-packages > "$index"
        gzip -n -c "$index" > "$index.gz"
    done
    rm all-packages
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin=LemonVox \
        -o APT::FTPArchive::Release::Label=LemonVox \
        -o APT::FTPArchive::Release::Suite=stable \
        -o APT::FTPArchive::Release::Codename=stable \
        -o "APT::FTPArchive::Release::Architectures=${sorted_architectures[*]}" \
        -o APT::FTPArchive::Release::Components=main \
        release dists/stable > Release
    mv Release dists/stable/Release
    gpg --batch --local-user "$fingerprint" --digest-algo SHA256 \
        --clearsign --output dists/stable/InRelease dists/stable/Release
    gpg --batch --export-options export-minimal --export "$fingerprint" > lemonvox-archive-keyring.gpg
    test -s lemonvox-archive-keyring.gpg
)
chmod 755 "$stage"
mv -nT "$stage" "$output"
if [[ -d $stage ]]; then
    echo "Repository output already exists: $output" >&2
    exit 2
fi
