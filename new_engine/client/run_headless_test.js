const puppeteer = require('puppeteer');

(async () => {
    const browser = await puppeteer.launch({
        headless: "new",
        args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-gpu']
    });
    const page = await browser.newPage();

    page.on('console', msg => console.log('BROWSER LOG:', msg.text()));

    await page.goto('http://localhost:8080');

    // Wait for the game to start and network to connect
    await new Promise(r => setTimeout(r, 5000));

    console.log("Taking screenshot...");
    await page.screenshot({ path: 'new_engine_screenshot.png' });

    await browser.close();
    console.log("Done.");
})();
