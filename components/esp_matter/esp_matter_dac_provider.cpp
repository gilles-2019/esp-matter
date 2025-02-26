// Copyright 2022 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#include <esp_fault.h>
#include <esp_log.h>
#include <esp_secure_cert_read.h>

#define TAG "dac_provider"

#if CONFIG_SEC_CERT_DAC_PROVIDER

namespace chip {
namespace DeviceLayer {

using namespace chip::Credentials;
using namespace chip::DeviceLayer::Internal;

namespace {
static constexpr uint32_t kDACPrivateKeySize = 32;
static constexpr uint32_t kDACPublicKeySize  = 65;
static constexpr uint8_t kPrivKeyOffset = 7;
static constexpr uint8_t kPubKeyOffset = 56;

CHIP_ERROR LoadKeypairFromRaw(ByteSpan privateKey, ByteSpan publicKey, Crypto::P256Keypair & keypair)
{
    Crypto::P256SerializedKeypair serializedKeypair;
    ReturnErrorOnFailure(serializedKeypair.SetLength(privateKey.size() + publicKey.size()));
    memcpy(serializedKeypair.Bytes(), publicKey.data(), publicKey.size());
    memcpy(serializedKeypair.Bytes() + publicKey.size(), privateKey.data(), privateKey.size());
    return keypair.Deserialize(serializedKeypair);
}
} // namespace

class SecureCertDACProvider: public Credentials::DeviceAttestationCredentialsProvider
{
public:
    SecureCertDACProvider() : Credentials::DeviceAttestationCredentialsProvider() {}

    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan & outBuffer) override
    {
        size_t certSize;
        ReturnErrorOnFailure(
            ESP32Config::ReadConfigValueBin(ESP32Config::kConfigKey_CertDeclaration, outBuffer.data(), outBuffer.size(), certSize));
        outBuffer.reduce_size(certSize);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetFirmwareInformation(MutableByteSpan & out_firmware_info_buffer) override
    {
        // We do not provide any FirmwareInformation.
        out_firmware_info_buffer.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan & outBuffer) override
    {
        char *dac_cert = NULL;
        uint32_t dac_len = 0;

        esp_err_t err = esp_secure_cert_get_device_cert(&dac_cert, &dac_len);
        if (err == ESP_OK && dac_cert != NULL && dac_len != 0)
        {
            ESP_FAULT_ASSERT(err == ESP_OK && dac_cert != NULL && dac_len != 0);
            memcpy(outBuffer.data(), dac_cert, dac_len);
            outBuffer.reduce_size(dac_len);
            esp_secure_cert_free_device_cert(dac_cert);
            return CHIP_NO_ERROR;
        }

        ESP_LOGE(TAG, "esp_secure_cert_get_device_cert failed err:%d", err);
        return CHIP_ERROR_INCORRECT_STATE;
    }

    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan & outBuffer) override
    {
        char *pai_cert = NULL;
        uint32_t pai_len = 0;

        esp_err_t err = esp_secure_cert_get_ca_cert(&pai_cert, &pai_len);
        if (err == ESP_OK && pai_cert != NULL && pai_len != 0)
        {
            ESP_FAULT_ASSERT(err == ESP_OK && pai_cert != NULL && pai_len != 0);
            memcpy(outBuffer.data(), pai_cert, pai_len);
            outBuffer.reduce_size(pai_len);
            esp_secure_cert_free_ca_cert(pai_cert);
            return CHIP_NO_ERROR;
        }

        ESP_LOGE(TAG, "esp_secure_cert_get_ca_cert failed err:%d", err);
        return CHIP_ERROR_INCORRECT_STATE;
    }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan & messageToSign, MutableByteSpan & outSignBuffer) override
    {
        Crypto::P256ECDSASignature signature;
        Crypto::P256Keypair keypair;
        char *sc_keypair = NULL;
        uint32_t sc_keypair_len = 0;

        VerifyOrReturnError(IsSpanUsable(outSignBuffer), CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(IsSpanUsable(messageToSign), CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(outSignBuffer.size() >= signature.Capacity(), CHIP_ERROR_BUFFER_TOO_SMALL);

        esp_err_t err = esp_secure_cert_get_priv_key(&sc_keypair, &sc_keypair_len);
        if (err == ESP_OK && sc_keypair != NULL && sc_keypair_len != 0)
        {
            ESP_FAULT_ASSERT(err == ESP_OK && sc_keypair != NULL && sc_keypair_len != 0);

            CHIP_ERROR chipError = LoadKeypairFromRaw(ByteSpan(reinterpret_cast<const uint8_t *>(sc_keypair + kPrivKeyOffset), kDACPrivateKeySize),
                                                    ByteSpan(reinterpret_cast<const uint8_t *>(sc_keypair + kPubKeyOffset), kDACPublicKeySize), keypair);
            if (chipError != CHIP_NO_ERROR)
            {
                esp_secure_cert_free_priv_key(sc_keypair);
                return chipError;
            }

            chipError = keypair.ECDSA_sign_msg(messageToSign.data(), messageToSign.size(), signature);
            if (chipError != CHIP_NO_ERROR)
            {
                esp_secure_cert_free_priv_key(sc_keypair);
                return chipError;
            }

            esp_secure_cert_free_priv_key(sc_keypair);
            chipError = CopySpanToMutableSpan(ByteSpan{ signature.ConstBytes(), signature.Length() }, outSignBuffer);
            return chipError;
        }

        ESP_LOGE(TAG, "esp_secure_cert_get_priv_key failed err:%d", err);
        return CHIP_ERROR_INCORRECT_STATE;
    }
};
} // namespace DeviceLayer
} // namespace chip

#endif // CONFIG_SEC_CERT_DAC_PROVIDER

namespace esp_matter {

// TODO: Need some method or mechanism to use the custom implementation along with the above three
chip::Credentials::DeviceAttestationCredentialsProvider * get_dac_provider(void)
{
#if CONFIG_SEC_CERT_DAC_PROVIDER
    static chip::DeviceLayer::SecureCertDACProvider instance;
    return &instance;
#elif CONFIG_FACTORY_PARTITION_DAC_PROVIDER
    static chip::DeviceLayer::ESP32FactoryDataProvider instance;
    return &instance;
#else // CONFIG_EXAMPLE_DAC_PROVIDER
    return chip::Credentials::Examples::GetExampleDACProvider();
#endif
}

} // namespace esp_matter
