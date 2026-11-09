#include <cassert>
#include <cstdint>
#include <string>
static int version_major=25;
static uint16_t USBToHost16(uint16_t n){return n;}
#define XYLog(...) do {} while(0)
struct OSString {static const char* withCString(const char* s){return s;}};
struct Device {struct Descriptor {uint16_t idVendor=0x0bda,idProduct=0x8922;} descriptor; bool valid=true,loaded=false;std::string name="Bluetooth Radio";const Descriptor* getDeviceDescriptor(){return valid?&descriptor:nullptr;}void setProperty(const char*,bool value){loaded=value;}void setName(const char* s){name=s;}};
class RealtekBluetoothFirmware {public:Device* m_pDevice;std::string firmware;void setProperty(const char*,const char* value){firmware=value;}void publishReg(bool,const char*);};
void RealtekBluetoothFirmware::publishReg(bool isSucceed, const char *fwName)
{
    m_pDevice->setProperty("FirmwareLoaded", isSucceed);
    if (isSucceed)
        setProperty("fw_name", OSString::withCString(fwName));
    if (version_major >= 21) {
        // Keep the real USB identity for this controller. Renaming it to
        // Apple's internal-controller name makes bluetoothd select the wrong
        // transport and fail to retrieve its Realtek vendor/product IDs.
        const auto *descriptor = m_pDevice->getDeviceDescriptor();
        const bool keepRealtekName = descriptor &&
            USBToHost16(descriptor->idVendor) == 0x0bda &&
            USBToHost16(descriptor->idProduct) == 0x8922;
        if (!keepRealtekName)
            m_pDevice->setName("Bluetooth USB Host Controller");
        else
            XYLog("preserving Realtek 0bda:8922 USB device name\n");
    }
}


int main(){
 Device d;RealtekBluetoothFirmware f;f.m_pDevice=&d;
 f.publishReg(true,"rtl8922au");assert(d.loaded&&d.name=="Bluetooth Radio"&&f.firmware=="rtl8922au");
 d.descriptor.idProduct=0x8852;f.publishReg(true,"other");assert(d.name=="Bluetooth USB Host Controller");
 d.name="Bluetooth Radio";d.descriptor.idProduct=0x8922;d.descriptor.idVendor=0x1234;f.publishReg(true,"other");assert(d.name=="Bluetooth USB Host Controller");
 d=Device();version_major=20;f.publishReg(true,"rtl8922au");assert(d.name=="Bluetooth Radio");
 d=Device();version_major=25;d.valid=false;f.publishReg(true,"unknown");assert(d.name=="Bluetooth USB Host Controller");
 d=Device();f.firmware="";f.publishReg(false,"unused");assert(!d.loaded&&f.firmware.empty()&&d.name=="Bluetooth Radio");
}
