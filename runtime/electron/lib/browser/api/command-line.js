//const EventEmitter = require('events').EventEmitter;
const binding = process._linkedBinding('electron_browser_commandline');
const ApiCommandLine = binding.ApiCommandLine;


module.exports = new ApiCommandLine();