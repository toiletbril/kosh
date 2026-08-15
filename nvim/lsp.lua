vim.filetype.add({
  extension = {
    sh = "sh",
    dash = "dash",
    bash = "bash",
    kosh = "kosh",
    shit = "shit",
  },
})

vim.treesitter.language.register("bash", {
  "sh",
  "dash",
  "bash",
  "kosh",
  "shit",
})

vim.lsp.config("kosh", {
  cmd = { "kosh", "--language-server" },
  filetypes = {
    "sh",
    "dash",
    "bash",
    "kosh",
    "shit",
  },
  root_dir = function(bufnr, on_dir)
    on_dir(vim.fs.root(bufnr, { ".git" }) or vim.fn.getcwd())
  end,
})

vim.lsp.enable("kosh")
