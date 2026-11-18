#!/bin/sh
# Makes the TLS certificates the pretend bridge (mock_hue.py) presents, and records the public-key
# pin of each as computed by the openssl command-line tool, which hue_test.cpp uses as known
# answers. The files are test fixtures, not secrets. They are already in certs/; run this only to
# make new ones (it needs the openssl command).
#
# Bridge ids use the IANA documentation MAC range 00:00:5e:00:53:xx (RFC 7042), written the way a
# bridge writes its id: the MAC with "fffe" in the middle.
#   a      self-signed EC P-256, CN = the mock's bridge id         (a bridge on older firmware)
#   b      as a, with a different key                              (the same bridge after an update)
#   rsa    self-signed RSA 2048, CN = the mock's bridge id
#   other  self-signed EC P-256, CN = another bridge's id          (a different device at the address)
#   chain  leaf with CN = the mock's bridge id, issued by a test CA (newer firmware presents a chain)
#   san    CN "Hue Bridge", the bridge id only as a DNS subjectAltName
set -eu
cd "$(dirname "$0")"
mkdir -p certs
cd certs
ID=00005efffe005301
OTHER=00005efffe0053ff
DAYS=7300

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout a.key -out a.pem -days $DAYS -subj "/C=NL/O=Test bridge/CN=$ID" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout b.key -out b.pem -days $DAYS -subj "/C=NL/O=Test bridge/CN=$ID" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout rsa.key -out rsa.pem -days $DAYS -subj "/C=NL/O=Test bridge/CN=$ID" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout other.key -out other.pem -days $DAYS -subj "/C=NL/O=Test bridge/CN=$OTHER" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout san.key -out san.pem -days $DAYS -subj "/C=NL/O=Test bridge/CN=Hue Bridge" -addext "subjectAltName=DNS:$ID" 2>/dev/null

# A test CA and a leaf it issued; chain.pem holds the leaf followed by the CA, as a server sends it.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.pem -days $DAYS -subj "/C=NL/O=Test bridge CA/CN=test-root-bridge" 2>/dev/null
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout chain.key -out chain.csr -subj "/C=NL/O=Test bridge/CN=$ID" 2>/dev/null
openssl x509 -req -in chain.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out chain-leaf.pem -days $DAYS 2>/dev/null
cat chain-leaf.pem ca.pem > chain.pem
rm -f chain.csr ca.srl ca.key chain-leaf.pem

# The pin of each certificate's public key, as libcurl's CURLOPT_PINNEDPUBLICKEY takes it.
: > pins.txt
for n in a b rsa other san chain; do
    pin=$(openssl x509 -in "$n.pem" -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | openssl base64 -A)
    echo "$n sha256//$pin" >> pins.txt
done
cat pins.txt
