import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://pnsk-lab.github.io',
  base: '/xyo-cpp/',
  integrations: [starlight({
    title: 'xyo-cpp',
    description: 'Scratch 3 の高速ネイティブランタイム',
    defaultLocale: 'ja',
    locales: { root: { label: '日本語', lang: 'ja' } },
    social: [{ icon: 'github', label: 'GitHub', href: 'https://github.com/pnsk-lab/xyo-cpp' }],
    customCss: ['./src/styles/custom.css'],
    sidebar: [
      { label: 'はじめに', items: ['getting-started', 'installation'] },
      { label: 'ユーザーガイド', items: ['guide/cli', 'guide/execution', 'guide/aot', 'guide/optimizations', 'guide/optimization', 'reference/running'] },
      { label: '内部構造', items: ['architecture/overview', 'architecture/wasm', 'reference/design'] },
      { label: '開発者向け', items: ['development/testing', 'reference/scratch-bench', 'reference/scratch-divergences', 'reference/wasm'] },
      { label: '研究・実験記録', autogenerate: { directory: 'research' }, collapsed: true },
    ],
  })],
});
