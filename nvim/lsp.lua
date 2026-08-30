--
-- This snipped allows you to use Koshka's shell language server in
-- Neovim 0.11+. Paste the code below into your configuration and you should be
-- good to go.
--

-- start koshka snippet
vim.filetype.add({
  extension = {
    sh = "sh",
    bash = "bash",
    kosh = "kosh",
    shit = "shit",
  },
})
vim.treesitter.language.register("bash", {
  "sh",
  "bash",
  "kosh",
  "shit",
})
vim.lsp.config("kosh", {
  cmd = { "kosh", "--language-server" },
  filetypes = {
    "sh",
    "bash",
    "kosh",
    "shit",
    "yaml",
    "yaml.ansible",
    "markdown",
    "dockerfile",
    "make",
    "json",
    "jsonc",
    "just",
    "spec",
  },
  root_dir = function(bufnr, on_dir)
    on_dir(vim.fs.root(bufnr, { ".git" })
      or vim.fs.root(bufnr, { "Makefile" })
      or vim.fn.getcwd())
  end,
})
vim.lsp.enable("kosh")
-- end koshka snippet
