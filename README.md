# mozc-skkserv

mozc-skkserv は、SKK クライアントから受け取ったひらがなの読みを [Mozc](https://github.com/google/mozc) で変換し、候補を返す Windows 用の SKK 辞書サーバです。

## 特徴

- ループバック(`127.0.0.1`)で待ち受け、外部サーバと通信せずローカルで変換します。
- UTF-8/EUC-JP に対応しています。

## 使い方

起動には `scripts\start-server.ps1` を使います。

```powershell
.\scripts\start-server.ps1                                # UTF-8, ポート 1178
.\scripts\start-server.ps1 -Port 1182 -Encoding euc-jp    # EUC-JP, ポート 1182
```

サーバは非表示のウィンドウで動作し、ログオフか再起動で止まります。

SKK クライアントには、ホストに `127.0.0.1`、ポートにサーバのポート、文字コードにサーバの `-Encoding` と同じものを指定します。

| `start-server.ps1` の引数 | 意味 | 既定値 |
|---|---|---|
| `-Port` | 待ち受けポート | `1178` |
| `-Encoding` | 要求と応答の文字コード(`utf-8` または `euc-jp`) | `utf-8` |
| `-LogDir` | ログの出力先。指定したときだけ、読みの長さと処理時間を記録する(読みそのものは記録しない) | なし |
| `-Exe` | 起動する実行ファイル | `dist\mozc_skkserv.exe` |

`mozc_skkserv.exe` に直接渡せるフラグは次のとおりです。

| フラグ | 意味 | 既定値 |
|---|---|---|
| `--skkserv_port` | 待ち受けポート | `1178` |
| `--skkserv_encoding` | `utf-8` または `euc-jp` | `utf-8` |
| `--skkserv_max_candidates` | 1 回の応答に含める候補数の上限 | `12` |
| `--skkserv_verbose` | 読みの長さ、候補数、処理時間を標準出力に出す | 無効 |

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

成果物は `dist\mozc_skkserv.exe` と `dist\licenses`(ライセンス文書)で、出力先は `-OutDir` で変えられます。

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
