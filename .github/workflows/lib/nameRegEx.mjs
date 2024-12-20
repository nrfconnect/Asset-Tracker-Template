export const nameRegEx = (version) =>
  new RegExp(
    `^hello\.nrfcloud\.com-${version}(\\+(?<configuration>[0-9A-Za-z.]+))?-thingy91x-nrf91-update-signed\.bin$`
  );
