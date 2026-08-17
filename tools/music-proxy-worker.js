/**
 * Cloudflare Worker - 喵伴音乐代理
 * ---------------------------------------------------------------
 * 用途：当你的路由器/热点屏蔽了 api.yaohud.cn（设备日志出现
 *       ESP_ERR_HTTP_CONNECT）时，用本 Worker 做中转。设备只连
 *       Cloudflare 的 *.workers.dev 域名（几乎不被墙），由 Worker
 *       在服务端（无封锁）去调 yaohud，再把 JSON 原样返回。
 *
 * 部署：
 *   1. 打开 https://dash.cloudflare.com/  -> Workers & Pages -> 创建
 *   2. 粘贴本文件，把下面 YAOHUD_KEY 换成你自己的 yaohud 密钥
 *   3. 部署后得到 https://<你的子域>.workers.dev
 *
 * 板子 Web 设置页（/settings）的「自定义接口」填：
 *   https://<你的子域>.workers.dev/music?msg={q}
 *   （{q} 会被自动替换为 URL 编码后的歌名）
 *
 * 返回格式与 yaohud 一致，板子解析逻辑会优先取 data.vipmusic.url 直链。
 */
const YAOHUD_KEY = "oIF7nJpFIcsgBBAkDmV";   // <-- 换成你的 yaohud 密钥
const YAOHUD_API = "https://api.yaohud.cn/api/music/wyvip?key=" + YAOHUD_KEY + "&msg=";

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const msg = url.searchParams.get("msg");
    if (!msg) {
      return new Response(JSON.stringify({ code: 400, msg: "missing msg" }),
        { headers: { "content-type": "application/json" } });
    }
    try {
      const api = YAOHUD_API + encodeURIComponent(msg) + "&n=1";
      const r = await fetch(api, { redirect: "follow" });
      const text = await r.text();
      return new Response(text, {
        headers: {
          "content-type": "application/json; charset=utf-8",
          "access-control-allow-origin": "*",
        },
      });
    } catch (e) {
      return new Response(JSON.stringify({ code: 502, msg: "proxy error: " + e.message }),
        { headers: { "content-type": "application/json" } });
    }
  },
};
