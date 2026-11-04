# xyo-cpp documentation

Astro + Starlight で作成した日本語ドキュメントサイトです。

```sh
cd site
bun install
bun run dev
bun run build
```

`src/content/docs/` のガイドを編集し、詳細な研究記録はリポジトリ直下の `docs/*.md` を編集します。ビルド時に `scripts/sync-docs.mjs` が既存資料を自動的に取り込みます。生成された `reference/` および `research/` は Git 管理対象外です。

サイトは GitHub Pages の `/xyo-cpp/` に配置する想定です。別のドメインで配信する場合は `astro.config.mjs` の `site`、`base` と内部リンクを調整してください。
