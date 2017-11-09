gcc -O3 -I/usr/local/include -I/usr/local/include/evhtp *.c -pthread -lm -levent -levhtp -lssl -levent_openssl -o daap-client
