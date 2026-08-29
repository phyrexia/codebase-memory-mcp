import { describe, expect, it } from "vitest";
import { detectLanguage, messages } from "./i18n";

describe("i18n", () => {
  it("detects Chinese from Accept-Language and falls back to English", () => {
    expect(detectLanguage("zh-CN,zh;q=0.9,en;q=0.8")).toBe("zh");
    expect(detectLanguage("de-DE,de;q=0.9")).toBe("en");
  });

  it("ranks Accept-Language by q rather than by presence", () => {
    // A bilingual visitor who prefers English but lists Chinese as a fallback.
    expect(detectLanguage("en-US,en;q=0.9,zh;q=0.5")).toBe("en");
    expect(detectLanguage("en,zh;q=0.1")).toBe("en");
    // Chinese still wins when it is actually preferred.
    expect(detectLanguage("zh;q=0.9,en;q=0.5")).toBe("zh");
  });

  it("treats q=0 as unacceptable", () => {
    expect(detectLanguage("zh;q=0, en")).toBe("en");
    expect(detectLanguage("zh;q=0")).toBe("en");
  });

  it("matches the language subtag, not a substring", () => {
    expect(detectLanguage("en-GB")).toBe("en");
    expect(detectLanguage("zh-Hant-TW")).toBe("zh");
  });

  it("honours the override and handles empty input", () => {
    expect(detectLanguage("zh-CN", "en")).toBe("en");
    expect(detectLanguage("en-US", "zh")).toBe("zh");
    expect(detectLanguage(null)).toBe("en");
    expect(detectLanguage("")).toBe("en");
  });

  it("keeps UI chrome messages in the catalog", () => {
    expect(messages.zh.tabs.projects).toBe("项目");
    expect(messages.zh.index.newIndex).toBe("新建索引");
    expect(messages.en.index.repositoryPath).toBe("Repository path");
    expect(messages.en.index.browseTimeout).toBe(
      "Folder listing timed out after 5 seconds. Showing the last successful listing.",
    );
    expect(messages.zh.index.browseTimeout).toBe(
      "文件夹列表请求在 5 秒后超时。仍显示上次成功的列表。",
    );
    expect(messages.en.index.browseFailed).toBe("Unable to browse this folder.");
    expect(messages.zh.index.browseFailed).toBe("无法浏览此文件夹。");
    expect(messages.en.index.browseTruncated).toBe(
      "Showing a partial folder list because a browse limit was reached.",
    );
    expect(messages.zh.index.browseTruncated).toBe(
      "由于达到浏览限制，当前仅显示部分文件夹。",
    );
  });
});
