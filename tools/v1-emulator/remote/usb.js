import { Adb, AdbDaemonTransport } from '@yume-chan/adb';
import AdbWebCredentialStore from '@yume-chan/adb-credential-web';
import { AdbDaemonWebUsbDeviceManager } from '@yume-chan/adb-daemon-webusb';

export async function connectV1() {
  const device = await AdbDaemonWebUsbDeviceManager.BROWSER.requestDevice({
    filters: [{ vendorId: 0x18d1, productId: 0xd002 }],
  });
  if (!device) return null;
  let timer;
  try {
    const connection = await device.connect();
    const authentication = AdbDaemonTransport.authenticate({ serial: device.serial, connection,
      credentialStore: new AdbWebCredentialStore('Sonix V1 Live USB') });
    const transport = await Promise.race([authentication, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('ADB timed out. Check the player and reconnect.')), 20000);
    })]);
    return new Adb(transport);
  } catch (error) {
    if (device.raw.opened) await device.raw.close().catch(() => {});
    throw error;
  } finally {
    clearTimeout(timer);
  }
}
