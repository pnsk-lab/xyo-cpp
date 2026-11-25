import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://pnsk-lab.github.io',
  base: '/xyo-cpp/',
  integrations: [starlight({
    title: 'xyo-cpp',
    description: 'Scratch 3 ランタイム（C++20 / LLVM JIT）',
    customCss: ['./src/styles/geometric.css', './src/styles/motion.css'],
    components: { Header: './src/components/DocHeader.astro' },
    head: [
      { tag: 'link', attrs: { rel: 'preconnect', href: 'https://fonts.googleapis.com' } },
      { tag: 'link', attrs: { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: 'anonymous' } },
      { tag: 'link', attrs: { rel: 'stylesheet', href: 'https://fonts.googleapis.com/css2?family=Geist:wght@400;500;550;600;650;700&family=Geist+Mono:wght@400;500&family=Noto+Sans+JP:wght@400;500;600;700&display=swap' } },
    ],
    defaultLocale: 'ja',
    locales: { root: { label: '日本語', lang: 'ja' } },
    social: [{ icon: 'github', label: 'GitHub', href: 'https://github.com/pnsk-lab/xyo-cpp' }],
    sidebar: [
      { label: 'はじめに', items: ['getting-started', 'installation', 'benchmarks'] },
      { label: 'ユーザーガイド', items: ['guide/cli', 'guide/execution', 'guide/aot', 'guide/optimizations', 'guide/optimization', 'guide/scratch-blocks', 'reference/running'] },
      { label: '内部構造', items: ['architecture/overview', 'architecture/wasm', 'reference/design'] },
      { label: '開発者向け', items: ['development/testing', 'reference/scratch-bench', 'reference/scratch-divergences', 'reference/wasm'] },
      { label: '研究・実験記録', autogenerate: { directory: 'research' }, collapsed: true },
    ],
  })],
});
