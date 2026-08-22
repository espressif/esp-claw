/// <reference types="astro/client" />
/// <reference path="../node_modules/@astrojs/starlight/virtual-internal.d.ts" />

interface ImportMetaEnv {
  readonly PUBLIC_FIRMWARE_SITE_URL?: string;
  readonly PUBLIC_DOC_SOURCECODE_REPO?: string;
  readonly PUBLIC_DOC_SOURCECODE_REFS?: string;
}
