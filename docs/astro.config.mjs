// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { fileURLToPath } from "url";

import starlightThemeNova from "starlight-theme-nova";
import astroD2 from "astro-d2";
import { remarkDocLinks } from "./src/plugins/remark-doc-links.ts";

const BASE = "/";

export default defineConfig({
  base: BASE,
  integrations: [
    astroD2(),
    starlight({
      title: "ESP-Claw Docs",
      favicon: "/favicon.ico",
      lastUpdated: true,
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/espressif/esp-claw",
        },
        {
          icon: "rocket",
          label: "Flash via Browser",
          href: "https://esp-claw.com/en/flash/",
        }
      ],
      sidebar: [
        {
          label: "Tutorial",
          translations: {
            en: "Tutorial",
            "zh-CN": "使用指南",
          },
          items: [
            { slug: "tutorial", label: "Welcome", translations: { en: "Welcome", "zh-CN": "欢迎" } }, // `index.mdx`
            { slug: "tutorial/supported-list" },
            { slug: "tutorial/bom" },
            { slug: "tutorial/assemble" },
            { slug: "tutorial/get-started" },
            { slug: "tutorial/web-config" },
            { slug: "tutorial/faq" },
            { slug: "tutorial/skills-lab" },
          ],
        },
        {
          label: "Reference",
          items: [
            {
              label: "Project Architecture",
              autogenerate: { directory: "reference-project" },
              translations: {
                en: "Project Architecture",
                "zh-CN": "项目架构",
              },
            },
            {
              label: "Core",
              autogenerate: { directory: "reference-core" },
              translations: {
                en: "Core",
                "zh-CN": "核心 Core",
              },
            },
            {
              label: "Capabilities",
              autogenerate: { directory: "reference-cap" },
              translations: {
                en: "Capabilities",
                "zh-CN": "能力 Capabilities",
              },
            },
          ],
          translations: {
            en: "Reference",
            "zh-CN": "开发参考",
          },
        },
      ],
      customCss: ["./src/styles/starlight.css"],
      defaultLocale: "en",
      locales: {
        en: {
          label: "English",
          lang: "en",
        },
        "zh-cn": {
          label: "中文",
          lang: "zh-CN",
        },
      },
      components: {
        SiteTitle: "./src/components/DocsSiteTitle.astro",
        Sidebar: "./src/components/Sidebar.astro",
      },
      plugins: [
        starlightThemeNova(),
      ],
    }),
  ],
  markdown: {
    remarkPlugins: [[remarkDocLinks, { base: BASE }]],
  },
  vite: {
    resolve: {
      alias: {
        "@": fileURLToPath(new URL("./src", import.meta.url)),
      },
    },
  },
});
