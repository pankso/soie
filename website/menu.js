/*
 * The phone menu is a checkbox (no script needed to open it): once one
 * of its links is followed, or the page is touched outside the header,
 * uncheck it so that it closes.
 */
(function () {
	var toggle = document.getElementById('nav-toggle');
	var header = document.getElementById('header');
	var links = document.querySelectorAll('#network a');
	var i;

	if (!toggle || !header)
		return;
	function close() {
		toggle.checked = false;
	}
	for (i = 0; i < links.length; i++)
		links[i].addEventListener('click', close);
	document.addEventListener('click', function (e) {
		if (toggle.checked && !header.contains(e.target))
			close();
	});
})();
