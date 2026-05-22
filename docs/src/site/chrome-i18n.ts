import type { Lang } from "./locales";

export interface ChromeStrings {
  navHome: string;
  navDocs: string;
  navSkillsLab: string;
  navGitHub: string;
  navCta: string;
  footerTerms: string;
  footerPrivacy: string;
  footerTermsLink: string;
  footerPrivacyLink: string;
}

const en: ChromeStrings = {
  navHome: "Home",
  navDocs: "Documents",
  navSkillsLab: "Skills Lab",
  navGitHub: "GitHub",
  navCta: "Get Started",
  footerTerms: "Terms",
  footerTermsLink: "https://www.espressif.com/en/content/terms-use",
  footerPrivacy: "Privacy",
  footerPrivacyLink: "https://www.espressif.com/en/content/privacy",
};

const zhCn: ChromeStrings = {
  navHome: "首页",
  navDocs: "文档",
  navSkillsLab: "Skills Lab",
  navGitHub: "GitHub",
  navCta: "开始使用",
  footerTerms: "条款",
  footerTermsLink: "https://www.espressif.com/zh-hans/content/terms-use",
  footerPrivacy: "隐私",
  footerPrivacyLink: "https://www.espressif.com/zh-hans/content/privacy",
};

const table: Record<Lang, ChromeStrings> = {
  en,
  "zh-cn": zhCn,
};

export function getChromeStrings(lang: Lang): ChromeStrings {
  return table[lang] ?? table.en;
}
