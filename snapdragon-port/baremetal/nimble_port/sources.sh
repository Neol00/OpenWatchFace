# nimble_port/sources.sh — sourced by build-owf-image*.sh: NimBLE host + mbedTLS
# crypto + our glue. Sets NIMBLE_INC (compiler include flags) and NIMBLE_SRCS.
# Set NIMBLE_TRANSPORT before sourcing to pick the HCI transport:
#   nimble_transport_smd.c   (default) shared memory to the WCNSS core - 8909w
#   nimble_transport_uart.c  H4 over blsp2_uart2 to a WCN3990 - Fossil Gen 6
NB=third_party/nimble
NIMBLE_INC="-I nimble_port -I $NB/nimble/include -I $NB/nimble/host/include \
  -I $NB/nimble/host/services/gap/include -I $NB/nimble/host/services/gatt/include \
  -I $NB/nimble/host/store/config/include -I $NB/nimble/host/util/include \
  -I $NB/nimble/transport/include -I $NB/porting/nimble/include -I $NB/porting/npl/freertos/include \
  -I third_party/mbedtls/include -DNIMBLE_NPL_OS_EXTRA_INCLUDE=\"npl_hw.h\" -DMBEDTLS_CONFIG_FILE=\"owf_mbedtls_config.h\""
NIMBLE_SRCS="$(ls $NB/nimble/host/src/*.c | grep -v 'ble_hs_stop\.c$' | tr '\n' ' ') \
  $NB/nimble/host/src/ble_hs_stop.c \
  $NB/nimble/host/services/gap/src/ble_svc_gap.c $NB/nimble/host/services/gatt/src/ble_svc_gatt.c \
  $NB/nimble/host/store/config/src/ble_store_config.c $NB/nimble/host/util/src/addr.c \
  $NB/nimble/transport/src/transport.c \
  $NB/porting/nimble/src/endian.c $NB/porting/nimble/src/mem.c $NB/porting/nimble/src/nimble_port.c \
  $NB/porting/nimble/src/os_mbuf.c $NB/porting/nimble/src/os_mempool.c $NB/porting/nimble/src/os_msys.c \
  $NB/porting/npl/freertos/src/npl_os_freertos.c \
  nimble_port/${NIMBLE_TRANSPORT:-nimble_transport_smd.c} nimble_port/nimble_glue.c"
MB=third_party/mbedtls/library
MBEDTLS_SRCS="$MB/aes.c $MB/cipher.c $MB/cipher_wrap.c $MB/cmac.c $MB/bignum.c $MB/bignum_core.c \
  $MB/ecp.c $MB/ecp_curves.c $MB/ecp_curves_new.c $MB/ecdh.c $MB/platform.c $MB/platform_util.c $MB/constant_time.c \
  $MB/gcm.c $MB/md.c $MB/sha1.c $MB/sha256.c $MB/sha512.c $MB/ecdsa.c $MB/rsa.c $MB/rsa_alt_helpers.c \
  $MB/asn1parse.c $MB/asn1write.c $MB/oid.c $MB/pk.c $MB/pk_wrap.c $MB/pkparse.c $MB/pk_ecc.c \
  $MB/pem.c $MB/base64.c $MB/ctr_drbg.c $MB/entropy.c $MB/entropy_poll.c \
  $MB/x509.c $MB/x509_crt.c $MB/ssl_tls.c $MB/ssl_msg.c $MB/ssl_client.c $MB/ssl_tls12_client.c $MB/ssl_ciphersuites.c \
  $MB/hmac_drbg.c $MB/bignum_mod.c $MB/bignum_mod_raw.c"
