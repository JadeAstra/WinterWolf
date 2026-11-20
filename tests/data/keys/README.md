# Keys in files

Throwaway keys OpenSSL 3.5 made for `tests/key_files.ww`, used for nothing else:

    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out rsa.pem   (and P-256, P-384, ED25519, X25519)
    openssl pkey -in K.pem -pubout -out K.pub
    openssl pkcs8 -topk8 -in K.pem -v2 aes-256-cbc -iter 2048 -passout pass:sesame -out K.enc
    openssl pkey -in rsa.pem -traditional -out rsa.trad
    openssl ec -in p256.pem -out p256.trad
    openssl rsa -in rsa.pem -RSAPublicKey_out -out rsa.rsapub
