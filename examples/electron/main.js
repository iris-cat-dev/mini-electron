const { app, BrowserWindow } = require('electron');

let mainWindow;

app.whenReady().then(async () => {
  if (!app.isReady()) throw new Error('app must be ready');

  mainWindow = new BrowserWindow({
    width: 900,
    height: 600,
    title: 'mini-electron API Demo',
    resizable: true,
    show: true
  });

  const bounds = mainWindow.getBounds();
  if (bounds.width !== 900 || bounds.height !== 600)
    throw new Error('BrowserWindow bounds mismatch');

  if (BrowserWindow.getAllWindows()[0] !== mainWindow)
    throw new Error('BrowserWindow registry mismatch');
  mainWindow.setTitle('mini-electron API Demo');
  if (mainWindow.getTitle() !== 'mini-electron API Demo')
    throw new Error('BrowserWindow title mismatch');

  await mainWindow.loadFile(`${__dirname}/index.html`);
  if (!mainWindow.webContents.getURL().endsWith('/index.html'))
    throw new Error('webContents URL mismatch');
});

app.on('window-all-closed', () => {
  app.quit();
});
