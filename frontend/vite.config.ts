import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

const baselineSecurityHeaders = {
  "X-Content-Type-Options": "nosniff",
  "X-Frame-Options": "DENY",
  "Referrer-Policy": "no-referrer",
  "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
};

const productionContentSecurityPolicy =
  "default-src 'self'; base-uri 'self'; object-src 'none'; frame-ancestors 'none'; " +
  "form-action 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; " +
  "img-src 'self' data: blob:; font-src 'self' data:; connect-src 'self'";

const developmentContentSecurityPolicy =
  "default-src 'self'; base-uri 'self'; object-src 'none'; frame-ancestors 'none'; " +
  "form-action 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; " +
  "img-src 'self' data: blob:; font-src 'self' data:; connect-src 'self' ws:";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  base: "./",
  server: {
    headers: {
      ...baselineSecurityHeaders,
      "Content-Security-Policy": developmentContentSecurityPolicy,
    },
  },
  preview: {
    headers: {
      ...baselineSecurityHeaders,
      "Content-Security-Policy": productionContentSecurityPolicy,
    },
  },
  build: {
    outDir: "../web/app",
    emptyOutDir: true,
    sourcemap: false,
  },
});
