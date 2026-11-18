// ==UserScript==
// @name         YouTube to ESP32 Lyric Sync
// @namespace    http://tampermonkey.net/
// @version      2.1
// @description  Sync YouTube / YT Music playback, thumbnail & controls with ESP32 via VPS server (bypasses HTTPS Mixed Content)
// @match        https://www.youtube.com/watch*
// @match        https://music.youtube.com/*
// @grant        GM_xmlhttpRequest
// @connect      192.168.100.80
// ==/UserScript==

(function () {
    'use strict';

    // Menggunakan HTTP via GM_xmlhttpRequest agar tidak diblokir Mixed Content oleh browser di halaman HTTPS YouTube
    const SERVER_STATE_URL = 'http://192.168.100.80:8765/yt/state';
    let inFlight = false;

    function getVideoId() {
        const params = new URLSearchParams(window.location.search);
        return params.get('v') || '';
    }

    function getTrackInfo() {
        const isYtMusic = location.hostname === 'music.youtube.com';
        if (isYtMusic) {
            const titleEl = document.querySelector('ytmusic-player-bar .title');
            const bylineEl = document.querySelector('ytmusic-player-bar .byline');
            const artist = bylineEl ? bylineEl.innerText.split('•')[0].trim() : '-';
            return {
                title: titleEl ? titleEl.innerText.trim() : '',
                channel: artist
            };
        }
        const titleEl = document.querySelector('h1.ytd-watch-metadata yt-formatted-string');
        const chanEl = document.querySelector('#owner #channel-name a');
        return {
            title: titleEl ? titleEl.innerText.trim() : '',
            channel: chanEl ? chanEl.innerText.trim() : '-'
        };
    }

    function handleCommand(cmd) {
        const video = document.querySelector('video');
        if (!video) return;

        if (cmd === 'toggle') {
            if (video.paused) video.play();
            else video.pause();
        } else if (cmd === 'prev') {
            const prevBtn = document.querySelector('.ytp-prev-button, ytmusic-player-bar .previous-button');
            if (video.currentTime <= 4 && prevBtn && prevBtn.getAttribute('aria-disabled') !== 'true') {
                prevBtn.click();
            } else {
                video.currentTime = Math.max(0, video.currentTime - 10);
            }
        } else if (cmd === 'next') {
            const nextBtn = document.querySelector('.ytp-next-button, ytmusic-player-bar .next-button');
            if (nextBtn && nextBtn.getAttribute('aria-disabled') !== 'true') {
                nextBtn.click();
            } else {
                video.currentTime = Math.min(video.duration || video.currentTime + 10, video.currentTime + 10);
            }
        }
    }

    setInterval(() => {
        if (inFlight) return;
        const video = document.querySelector('video');
        const info = getTrackInfo();
        const videoId = getVideoId();
        if (!video || !info.title || !Number.isFinite(video.duration) || video.duration <= 0) return;

        const payload = {
            videoId: videoId,
            title: info.title,
            channel: info.channel,
            time: Number.isFinite(video.currentTime) ? video.currentTime : 0,
            duration: Number.isFinite(video.duration) ? video.duration : 0,
            paused: video.paused
        };

        inFlight = true;
        GM_xmlhttpRequest({
            method: 'POST',
            url: SERVER_STATE_URL,
            headers: { 'Content-Type': 'application/json' },
            data: JSON.stringify(payload),
            timeout: 2000,
            onload: (res) => {
                inFlight = false;
                try {
                    const data = JSON.parse(res.responseText);
                    if (data && Array.isArray(data.cmds)) {
                        for (const cmd of data.cmds) {
                            handleCommand(cmd);
                        }
                    }
                } catch (_) {}
            },
            onerror: () => { inFlight = false; },
            ontimeout: () => { inFlight = false; }
        });
    }, 300);
})();
