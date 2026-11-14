// Phone side. ES5 only. Makes no network requests: it shows the settings page and keeps a
// copy of the nightly baseline in this app's local storage so a wiped watch can get it back.
var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var BACKUP_KEY = 'ebb-nights-v1';
var NIGHT_BYTES = 8;

function log(msg) {
  console.log('[ebb] ' + msg);
}

function readBackup() {
  try {
    var bytes = JSON.parse(localStorage.getItem(BACKUP_KEY) || 'null');
    if (bytes && bytes.length && bytes.length % NIGHT_BYTES === 0) {
      return bytes;
    }
  } catch (e) {
    log('backup unreadable: ' + e);
  }
  return null;
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) {
    return;
  }
  var raw = clay.getSettings(e.response, false);
  var value = function (key, fallback) {
    return raw[key] && raw[key].value !== undefined ? raw[key].value : fallback;
  };
  var reset = !!value('ResetBaseline', false);
  var message = {
    Intensity: parseInt(value('Intensity', '1'), 10),
    HrvUnits: parseInt(value('HrvUnits', '0'), 10),
    MaxHr: parseInt(value('MaxHr', 185), 10)
  };

  if (reset) {
    // Drop the phone copy first so the watch's empty baseline is not answered with it.
    localStorage.removeItem(BACKUP_KEY);
    message.ResetBaseline = 1;
    // The toggle is a one-shot action: do not leave it switched on for the next visit.
    try {
      var stored = JSON.parse(localStorage.getItem('clay-settings') || '{}');
      stored.ResetBaseline = false;
      localStorage.setItem('clay-settings', JSON.stringify(stored));
    } catch (err) {
      log('could not clear reset toggle: ' + err);
    }
  }

  Pebble.sendAppMessage(message, function () {
    log('settings sent');
  }, function (err) {
    log('settings not delivered: ' + JSON.stringify(err));
  });
});

Pebble.addEventListener('appmessage', function (e) {
  var payload = e.payload || {};
  if (payload.NightsCount === undefined) {
    return;
  }
  if (payload.NightsCount > 0 && payload.NightsBlob) {
    localStorage.setItem(BACKUP_KEY, JSON.stringify(payload.NightsBlob));
    log('backed up ' + payload.NightsCount + ' nights');
    return;
  }
  // The watch has no baseline. If the phone has one, hand it back.
  var backup = readBackup();
  if (backup) {
    Pebble.sendAppMessage({ RestoreNights: backup }, function () {
      log('restored ' + (backup.length / NIGHT_BYTES) + ' nights to the watch');
    }, function (err) {
      log('restore not delivered: ' + JSON.stringify(err));
    });
  }
});

Pebble.addEventListener('ready', function () {
  log('ready');
});
