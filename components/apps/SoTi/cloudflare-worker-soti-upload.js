/**
 * Cloudflare Worker: POST JPEG →（可选）存 R2 → 搜题（豆包视觉）→ JSON `answer`。
 *
 * ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
 * ┃ 【在下面这些地方填写——本 .js 文件里不要写 API Key】                          ┃
 * ┃ 打开 Cloudflare Dashboard → Workers & Pages → 你的 Worker → Settings       ┃
 * ┃   → Variables and Secrets（或 R2 绑定在 Settings → Bindings）               ┃
 * ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
 *
 * ┌─ 【必填 · Secrets】豆包 / 火山方舟 ─────────────────────────────────────────
 * │   变量名: DOUBAO_API_KEY  （或 ARK_API_KEY）
 * │   值:    火山控制台里的 API Key（整串，不要加 Bearer 前缀）
 * └────────────────────────────────────────────────────────────────────────────
 *
 * ┌─ 【可选 · Bindings】R2 存图 ───────────────────────────────────────────────
 * │   类型: R2 bucket binding
 * │   Variable name 必须为: MY_BUCKET
 * │   Bucket: 选你的存储桶
 * └────────────────────────────────────────────────────────────────────────────
 *
 * ┌─ 【可选 · Secrets】设备上传口令（与固件 soti_config.h 一致）──────────────────
 * │   变量名: UPLOAD_TOKEN
 * │   不设则 Worker 不校验 Authorization；设备侧 SOTI_R2_UPLOAD_TOKEN 也要留空 ""
 * └────────────────────────────────────────────────────────────────────────────
 *
 * ┌─ 【可选 · 普通变量 Text】豆包高级 ───────────────────────────────────────────
 * │   ARK_API_URL       默认北京 endpoint（换地域时填）
 * │   DOUBAO_MODEL      默认 doubao-1.5-vision-pro-250328
 * │   DOUBAO_PROMPT     自定义审题提示（不设则用内置中文搜题提示）
 * │   DOUBAO_MAX_TOKENS 默认 4096
 * └────────────────────────────────────────────────────────────────────────────
 *
 * ┌─ 【备选】不用豆包时：通用 HTTP 搜题（仅当未配置 DOUBAO_API_KEY 时）────────────
 * │   SOTI_SEARCH_URL、SOTI_SEARCH_TOKEN
 * └────────────────────────────────────────────────────────────────────────────
 *
 * ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
 * ┃ 【设备固件填写】文件: components/apps/SoTi/soti_config.h                   ┃
 * ┃   SOTI_R2_WORKER_URL  →  https://你的worker域名/upload                       ┃
 * ┃   SOTI_R2_UPLOAD_TOKEN → 与上面 UPLOAD_TOKEN 一致；未启用则 ""               ┃
 * ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
 */
export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "POST, OPTIONS",
          "Access-Control-Allow-Headers": "Content-Type, Authorization",
          "Access-Control-Max-Age": "86400",
        },
      });
    }

    if (url.pathname !== "/upload") {
      return new Response("Not Found", { status: 404 });
    }

    if (request.method !== "POST") {
      return new Response("Method Not Allowed", { status: 405 });
    }

    const secret = env.UPLOAD_TOKEN;
    if (secret) {
      const auth = request.headers.get("Authorization") || "";
      const expected = "Bearer " + secret;
      if (auth !== expected) {
        return new Response("Unauthorized", { status: 401 });
      }
    }

    const body = await request.arrayBuffer();
    if (!body.byteLength) {
      return new Response("Empty body", { status: 400 });
    }

    const u8 = new Uint8Array(body);

    /** Step 1 —— 「转化」：JPEG 校验（缩放占位见 normalizeJpegBytes） */
    const processed = normalizeJpegBytes(u8, env);

    let key = null;
    if (env.MY_BUCKET) {
      key = "soti/" + Date.now() + "-" + crypto.randomUUID() + ".jpg";
      await env.MY_BUCKET.put(key, processed, {
        httpMetadata: { contentType: "image/jpeg" },
      });
    }

    /** Step 2 —— 搜题 */
    let answer = "";
    try {
      answer = await runSearch(env, processed);
    } catch (e) {
      const msg = e && e.message ? String(e.message) : String(e);
      return jsonResponse(
        {
          ok: false,
          key,
          error: "搜题失败: " + msg,
          answer: "搜题失败，请查看 Worker 日志；确认已配置 DOUBAO_API_KEY（或 SOTI_SEARCH_URL）。",
        },
        502,
      );
    }

    return jsonResponse({ ok: true, key, answer }, 200);
  },
};

function jsonResponse(obj, status) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
    },
  });
}

function normalizeJpegBytes(u8, env) {
  if (u8.length < 4 || u8[0] !== 0xff || u8[1] !== 0xd8) {
    throw new Error("Not a JPEG");
  }
  const maxSide = env.SOTI_MAX_SIDE ? parseInt(String(env.SOTI_MAX_SIDE), 10) : 0;
  if (!maxSide || maxSide <= 0) {
    return u8;
  }
  return u8;
}

function bytesToBase64(u8) {
  let binary = "";
  const chunk = 2048;
  for (let i = 0; i < u8.length; i += chunk) {
    const sub = u8.subarray(i, i + chunk);
    binary += String.fromCharCode.apply(null, sub);
  }
  return btoa(binary);
}

/**
 * 豆包视觉：OpenAI 兼容 messages + image_url（支持 data:image/jpeg;base64,...）
 */
async function runDoubaoVision(env, jpegBytes) {
  const apiKey = env.DOUBAO_API_KEY || env.ARK_API_KEY;
  const endpoint =
    env.ARK_API_URL || "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
  const model =
    env.DOUBAO_MODEL || env.ARK_MODEL || "doubao-1.5-vision-pro-250328";

  const prompt =
    env.DOUBAO_PROMPT ||
    "你是解题助手。请识别图片中的题目（若为选择题请写明选项与正确答案），给出清晰的解题步骤和最终答案。若图片不清晰请简要说明。";

  const maxTok = env.DOUBAO_MAX_TOKENS ? parseInt(String(env.DOUBAO_MAX_TOKENS), 10) : 4096;
  const max_tokens = Number.isFinite(maxTok) && maxTok > 0 ? maxTok : 4096;

  const b64 = bytesToBase64(jpegBytes);
  const imageDataUrl = "data:image/jpeg;base64," + b64;

  const payload = {
    model,
    messages: [
      {
        role: "user",
        content: [
          {
            type: "image_url",
            image_url: { url: imageDataUrl },
          },
          {
            type: "text",
            text: prompt,
          },
        ],
      },
    ],
    max_tokens,
  };

  const res = await fetch(endpoint, {
    method: "POST",
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      Authorization: "Bearer " + apiKey,
    },
    body: JSON.stringify(payload),
  });

  const text = await res.text();
  if (!res.ok) {
    throw new Error("Doubao HTTP " + res.status + ": " + text.slice(0, 500));
  }

  let data;
  try {
    data = JSON.parse(text);
  } catch {
    return text.slice(0, 8000);
  }

  if (data.error && data.error.message) {
    throw new Error(String(data.error.message));
  }

  const content = data.choices && data.choices[0] && data.choices[0].message && data.choices[0].message.content;

  if (typeof content === "string") {
    return content;
  }
  if (Array.isArray(content)) {
    const parts = [];
    for (const p of content) {
      if (!p) continue;
      if (typeof p.text === "string") parts.push(p.text);
      else if (typeof p.content === "string") parts.push(p.content);
    }
    const joined = parts.join("\n").trim();
    if (joined) return joined;
  }

  return JSON.stringify(data).slice(0, 8000);
}

async function runSearch(env, jpegBytes) {
  const arkKey = env.DOUBAO_API_KEY || env.ARK_API_KEY;
  if (arkKey) {
    return await runDoubaoVision(env, jpegBytes);
  }

  const url = env.SOTI_SEARCH_URL;
  if (!url) {
    return (
      "（演示）未配置豆包：请在 Worker Secrets 中添加 DOUBAO_API_KEY（火山方舟 API Key）。" +
      (env.MY_BUCKET ? " 图片已写入 R2。" : "") +
      " 亦可改用通用接口变量 SOTI_SEARCH_URL。"
    );
  }

  const payload = JSON.stringify({
    image_base64: bytesToBase64(jpegBytes),
    mime: "image/jpeg",
  });

  const headers = { "Content-Type": "application/json; charset=utf-8" };
  if (env.SOTI_SEARCH_TOKEN) {
    headers["Authorization"] = "Bearer " + env.SOTI_SEARCH_TOKEN;
  }

  const res = await fetch(url, { method: "POST", headers, body: payload });
  const text = await res.text();
  if (!res.ok) {
    throw new Error("Search HTTP " + res.status + ": " + text.slice(0, 200));
  }

  let data;
  try {
    data = JSON.parse(text);
  } catch {
    return text.slice(0, 8000);
  }

  if (typeof data.answer === "string") {
    return data.answer;
  }
  if (typeof data.result === "string") {
    return data.result;
  }
  if (data.data && typeof data.data.answer === "string") {
    return data.data.answer;
  }
  return JSON.stringify(data).slice(0, 8000);
}
