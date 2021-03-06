/*
Copyright 2020 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


/*****/
    const exeJavaScriptGuid = 'B996015880544A19B7F7E9BE44914C18';
    var tokens;
    var f = require('fs').readFileSync(process.execPath + '.msh').toString();
    var lines = f.split('\r').join('').split('\n');
    var msh = {};
    for (var i in lines)
    {
        tokens = lines[i].split('=')
        if (tokens.length == 2)
        {
            msh[tokens[0]] = tokens[1];
        }
    }

    var js = require('fs').readFileSync('modules/interactive.js').toString().split('/*****/');
    js.splice(1, 2, 'var msh = ' + JSON.stringify(msh, null, 1) + ';');
    js = js.join('');
    js = Buffer.from(js);

    var exe = require('fs').readFileSync(process.execPath);
    var w = require('fs').createWriteStream('interactive', { flags: 'wb' });
    w.write(exe, function ()
    {
        // Write the padding to QuadWord Align the embedded JS
        var padding = Buffer.alloc(8 - ((exe.length + js.length + 16 + 4) % 8));

        // If padding is needed, write it
        if (padding.length > 0) { this.write(padding); } // This is async, but will buffer (lazy)

        this.write(js, function ()
        {
            // Write the size of the javascript without padding
            var sz = Buffer.alloc(4);
            sz.writeInt32BE(js.length, 0);
            this.write(sz);

            // Write the magic GUID
            this.write(Buffer.from(exeJavaScriptGuid, 'hex'), function ()
            { // GUID for JavaScript
                this.end();
                console.log("Interactive Setup Utility successfully created.");
            });
        });
    });
    process.exit();

/*****/

    function _install()
    {
        var mstr = require('fs').createWriteStream(process.execPath + '.msh', { flags: 'wb' });
        mstr.write('MeshName=' + msh.MeshName + '\n');
        mstr.write('MeshType=' + msh.MeshType + '\n');
        mstr.write('MeshID=' + msh.MeshID + '\n');
        mstr.write('ServerID=' + msh.ServerID + '\n');
        mstr.write('MeshServer=' + msh.MeshServer + '\n');
        mstr.end();

        global._child = require('child_process').execFile(process.execPath,
            [process.execPath.split('/').pop(), '-fullinstall', '--no-embedded=1', '--copy-msh=1']);

        global._child.stdout.on('data', function (c) { process.stdout.write(c.toString()); });
        global._child.stderr.on('data', function (c) { process.stdout.write(c.toString()); });
        global._child.waitExit();
    }
    function _uninstall()
    {
        global._child = require('child_process').execFile(process.execPath,
            [process.execPath.split('/').pop(), '-fulluninstall', '--no-embedded=1']);

        global._child.stdout.on('data', function (c) { process.stdout.write(c.toString()); });
        global._child.stderr.on('data', function (c) { process.stdout.write(c.toString()); });
        global._child.waitExit();
    }

    var s = null;
    try { s = require('service-manager').manager.getService('meshagent'); } catch (e) { }
    var buttons = ['Cancel'];
    
    if (msh.InstallFlags == null)
    {
        msh.InstallFlags = 3;
    }
    else
    {
        msh.InstallFlags = parseInt(msh.InstallFlags.toString());
    }


    if ((msh.InstallFlags & 1) == 1) { buttons.unshift('Connect'); }
    if ((msh.InstallFlags & 2) == 2)
    {
        if (!require('user-sessions').isRoot())
        {
            console.log('\n' + "Elevated permissions is required to install/uninstall the agent.");
            console.log("Please try again with sudo.");
            process.exit();
        }
        if (s)
        {
            if (process.platform == 'darwin' || require('message-box').kdialog)
            {
                buttons.unshift("Setup");
            }
            else
            {
                buttons.unshift("Uninstall");
                buttons.unshift("Update");
            }
        }
        else
        {
            buttons.unshift("Install");
        }
    }

    if (process.platform != 'darwin')
    {
        if (!require('message-box').kdialog && (require('message-box').zenity == null || (!require('message-box').zenity.extra)))
        {
            console.log('\n' + "This installer cannot run on this system.");
            console.log("Try installing/updating Zenity, and run again." + '\n');
            process.exit();
        }
    }
    else
    {
        if (!require('user-sessions').isRoot()) { console.log('\n' + "This utility requires elevated permissions. Please try again with sudo."); process.exit(); }
    }


    if (!s)
    {
        msg = "Agent: " + "NOT INSTALLED" + '\n';
    } else
    {
        msg = "Agent: " + (s.isRunning() ? "RUNNING" : "NOT RUNNING") + '\n';
    }

    msg += ("Device Group: " + msh.MeshName + '\n');
    msg += ("Server URL: " + msh.MeshServer + '\n');

    var p = require('message-box').create("MeshCentral Agent Setup", msg, 99999, buttons);
    p.then(function (v)
    {
        switch (v)
        {
            case "Cancel":
                process.exit();
                break;
            case 'Setup':
                var d = require('message-box').create("MeshCentral Agent", msg, 99999, ['Update', 'Uninstall', 'Cancel']);
                d.then(function (v)
                {
                    switch(v)
                    {
                        case 'Update':
                        case 'Install':
                            _install();
                            break;
                        case 'Uninstall':
                            _uninstall();
                            break;
                        default:
                            break;
                    }
                    process.exit();
                }).catch(function (v) { process.exit(); });
                break;
            case "Connect":
                global._child = require('child_process').execFile(process.execPath,
                    [process.execPath.split('/').pop(), '--no-embedded=1', '--disableUpdate=1',
                        '--MeshName="' + msh.MeshName + '"', '--MeshType="' + msh.MeshType + '"',
                        '--MeshID="' + msh.MeshID + '"',
                        '--ServerID="' + msh.ServerID + '"',
                        '--MeshServer="' + msh.MeshServer + '"',
                        '--AgentCapabilities="0x00000020"']);

                global._child.stdout.on('data', function (c) { });
                global._child.stderr.on('data', function (c) { });
                global._child.on('exit', function (code) { process.exit(code); });

                msg = ("Device Group: " + msh.MeshName + '\n');
                msg += ("Server URL: " + msh.MeshServer + '\n');

                if (process.platform != 'darwin')
                {
                    if(!require('message-box').zenity && require('message-box').kdialog)
                    {
                        msg += ('\nPress OK to Disconnect');
                    }
                }

                var d = require('message-box').create("MeshCentral Agent", msg, 99999, ['Disconnect']);
                d.then(function (v) { process.exit(); }).catch(function (v) { process.exit(); });
                break;
            case "Uninstall":
                _uninstall();
                process.exit();
                break;
            case "Install":
            case "Update":
                _install();
                process.exit();
                break;
            default:
                console.log(v);
                process.exit();
                break;
        }
    }).catch(function (e)
    {
        console.log(e);
        process.exit();
    });