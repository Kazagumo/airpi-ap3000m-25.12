'use strict';
'require view';
'require uci';

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,

	load() {
		return uci.load('at-webserver');
	},

	render() {
		return E('iframe', {
			src: 'http://' + window.location.hostname + '/5700/',
			style: 'width: 100%; min-height: 100vh; border: none; border-radius: 3px;'
		});
	}
});