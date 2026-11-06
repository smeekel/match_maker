#include "cert.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <stdexcept>

namespace matchmaker::test
{

void write_test_certificate(const std::filesystem::path& certificate,
                            const std::filesystem::path& key)
{
  EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048);
  if( !pkey )
  {
    throw std::runtime_error("failed to generate a test key");
  }
  X509* x509 = X509_new();
  if( !x509 )
  {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("failed to allocate a test certificate");
  }
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_getm_notBefore(x509), 0);
  X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60 * 24);
  X509_set_pubkey(x509, pkey);
  X509_NAME* name = X509_NAME_new();
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_subject_name(x509, name);
  X509_set_issuer_name(x509, name);
  X509_NAME_free(name);
  if( X509_sign(x509, pkey, EVP_sha256()) == 0 )
  {
    X509_free(x509);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("failed to sign the test certificate");
  }
  BIO* key_bio = BIO_new_file(key.string().c_str(), "wb");
  if( !key_bio ||
      PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1 )
  {
    BIO_free(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("failed to write the test key");
  }
  BIO_free(key_bio);
  BIO* cert_bio = BIO_new_file(certificate.string().c_str(), "wb");
  if( !cert_bio || PEM_write_bio_X509(cert_bio, x509) != 1 )
  {
    BIO_free(cert_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("failed to write the test certificate");
  }
  BIO_free(cert_bio);
  X509_free(x509);
  EVP_PKEY_free(pkey);
}

} // namespace matchmaker::test
