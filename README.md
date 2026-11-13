# mozc-skkserv

[Mozc](https://github.com/google/mozc) で変換する Windows 用の SKK 辞書サーバです。

外部サーバと通信せずローカルで変換します。UTF-8/EUC-JP に対応しています。

## 使い方

```powershell
.\mozc_skkserv.exe                                # UTF-8, ポート 1178
.\mozc_skkserv.exe --port=1182 --encoding=euc-jp  # EUC-JP, ポート 1182
```

| フラグ | 意味 | 既定値 |
|---|---|---|
| `--port` | 待ち受けポート | `1178` |
| `--encoding` | 要求と応答の文字コード(`utf-8` または `euc-jp`) | `utf-8` |
| `--max_candidates` | 1 回の応答に含める候補数の上限 | `12` |
| `--verbose` | 問い合わせのたびに、読みの長さ、候補数、処理時間を出す | 無効 |

ログオン時、ウィンドウを出さずに起動するには、`shell:startup` のフォルダに、ショートカットを作ります。リンク先を `powershell.exe` にし、引数を次のようにします(`C:\path\to` は、`mozc_skkserv.exe` を置いた場所)。

```
-NoProfile -WindowStyle Hidden -Command "Start-Process 'C:\path\to\mozc_skkserv.exe' -ArgumentList '--port=1178' -WindowStyle Hidden"
```

SKK クライアントには、ホストに `127.0.0.1`、ポートにサーバのポート、文字コードにサーバの `--encoding` と同じものを指定します。

## プロトコル

実装しているのは、skkserv プロトコルのうち SKK クライアントが使う部分です。

| 要求 | 応答 |
|---|---|
| `1<読み> ` | 見つかった場合 `1/<候補>/<候補>/.../\n`。見つからない場合 `4\n` |
| `2` | バージョン(`mozc-skkserv/0.1.0 `) |
| `3` | ホスト情報 |
| `4<読み> ` | `4\n`(補完は未対応) |
| `0` | 接続を閉じる |

次の場合の応答は `4`(見つからない場合と同じ)です。

- 読みが、ひらがな(U+3041〜U+3096)、U+309B〜U+309E、長音符(U+30FC)以外の文字を含む場合。送りがなのある語を引くときの `おくr` のように、アルファベットを含む読みも、これに当たります。
- 読みが 80 文字を超える場合。
- 要求が不正な UTF-8(EUC-JP モードでは不正な EUC-JP)の場合。

要求が 4,096 バイトを超えても空白が来ない接続は閉じられます。同時接続は 16 本までです。

## EUC-JP モード

EUC-JP モードで返る候補は、JIS X 0208、ASCII、半角カタカナ(`8E xx`)で表せるものだけです。絵文字、JIS X 0212(`8F`)、NEC・IBM の拡張文字を含む候補は、応答から除かれます。

文字コードの変換は、`overlay\mozc_inproc\jisx0208_table.h` の変換表で行います。Windows と JIS X 0208 で割り当てが異なる 6 つの区点では、JIS の文字(U+301C の波ダッシュなど)と Windows の文字(U+FF5E の全角チルダなど)のどちらも、その区点に変換されます。

## ビルド

Mozc 本体のファイルは変更せず、`overlay\mozc_inproc` を Mozc のソースツリーにコピーして、Bazel でビルドします。

必要なもの:

- Windows の開発者モード、または管理者権限のシェル(Mozc のビルドがシンボリックリンクを使うため)
- C++ ツールと Windows SDK、ATL を含む Visual Studio
- [Bazelisk](https://github.com/bazelbuild/bazelisk)(PATH に置くか、`-BazeliskDir` で場所を指定します)

```powershell
git submodule update --init
cd mozc\src
python build_tools/update_deps.py --noqt --nowix
cd ..\..
.\scripts\build.ps1
```

`dist\` に`mozc_skkserv.exe` とライセンス文書(`LICENSE`、`LICENSE.mozc`、`THIRD_PARTY_NOTICES.txt`)が作られます。出力先は `-OutDir` で変えられます。

## テスト

```powershell
python tests/test_server.py [mozc_skkserv.exe のパス]
```

テストは、サーバを自分で起動し、プロトコル、入力の上限、EUC-JP の出力を確認します。

EUC-JP の変換表の検査は、次のとおりです。

```powershell
.\scripts\build.ps1 -Targets euc_jp_check -OutDir tmp\check
python tests/test_euc_jp.py tmp\check\euc_jp_check.exe
```

## ライセンス

mozc-skkserv のライセンスは BSD-3-Clause です([LICENSE](LICENSE))。

実行ファイルには、Mozc と Mozc の辞書データ、いくつかのライブラリが含まれます。それぞれのライセンスは、[LICENSE.mozc](LICENSE.mozc) と [licenses/THIRD_PARTY_NOTICES.txt](licenses/THIRD_PARTY_NOTICES.txt) にあります。

このプロジェクトは Google および Mozc とは関係のない、個人によるものです。
