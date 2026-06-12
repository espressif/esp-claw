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
  site: "https://esp-claw.com",
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
            { slug: "tutorial/first-interactions", translations: { en: "First interactions", "zh-CN": "上手体验" } },
            { slug: "tutorial/web-config" },
            { slug: "tutorial/skills-lab" },
            { slug: "tutorial/faq" },
          ],
        },
        {
          label: "Reference",
          items: [
            {
              label: "Project Architecture",
              translations: {
                en: "Project Architecture",
                "zh-CN": "项目架构",
              },
              items: [
                { autogenerate: { directory: "reference-project" } },
              ]
            },
            {
              label: "Core",
              translations: {
                en: "Core",
                "zh-CN": "核心 Core",
              },
              items: [
                { autogenerate: { directory: "reference-core" } },
              ]
            },
            {
              label: "Capabilities",
              translations: {
                en: "Capabilities",
                "zh-CN": "能力 Capabilities",
              },
              items: [
                { slug: "reference-cap"},
                { slug: "reference-cap/implement-capability" },
                { slug: "reference-cap/cap-im-platform" },
                { slug: "reference-cap/cap-skill" },
                { slug: "reference-cap/cap-agent-mgr" },
                { slug: "reference-cap/cap-llm-inspect" },
                { slug: "reference-cap/cap-files" },
                { slug: "reference-cap/cap-system" },
                { slug: "reference-cap/cap-scheduler" },
                { slug: "reference-cap/cap-http-request" },
                { slug: "reference-cap/cap-web-search" },
                { slug: "reference-cap/cap-router-mgr" },
                { slug: "reference-cap/cap-mcp" },
                { slug: "reference-cap/cap-lua" },
                { slug: "reference-cap/lua-modules" },
              ]
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
    gfm: true,
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
